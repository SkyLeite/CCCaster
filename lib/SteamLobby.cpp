#ifdef ENABLE_STEAM

#include "SteamLobby.hpp"
#include "SteamManager.hpp"
#include "SteamSocket.hpp"   // steamAddr()
#include "Logger.hpp"

#include <windows.h>         // Sleep
#include <cctype>

using namespace std;


// Bounded pump loop: ~5s like the standalone steam() UI used. There is no EventManager loop
// during the lobby menu, so we drive SteamManager's manual dispatch directly here.
#define SETTLE_TRIES ( 500 )
#define SETTLE_SLEEP_MS ( 10 )


SteamLobby::SteamLobby ( ILobbyBackend::Owner* owner )
{
    this->owner = owner;
    mode = MENU;
}

SteamLobby::~SteamLobby()
{
    SteamManager::get().leaveLobby();
    // Release the ref taken by the MainUi backend factory. Guarded no-op if MainApp already
    // shut Steam down before launching the game (the match path).
    SteamManager::get().deref();
}

void SteamLobby::start()
{
    // Steam was already ref()'d by the MainUi factory. Nothing async to start; the UI opens at
    // the top-level lobby MENU.
    mode = MENU;
    connectionSuccess = true;
}

bool SteamLobby::isRunning()
{
    // Steam may be shut down mid-session by MainApp when a match launches; that ends the lobby.
    return SteamManager::get().isInitialized();
}

void SteamLobby::stop()
{
    SteamManager::get().leaveLobby();
}


// ---- menu data (called by MainUi while it holds entryMutex; must NOT re-lock) ----

vector<string> SteamLobby::getMenu()
{
    switch ( mode )
    {
        case MENU:
            // No "Default Lobby" entry for Steam (that relay-only mode has no Steam analog).
            return { "Public Lobbies", "Create Lobby", "Enter Lobby Code" };
        case CONCERTO_BROWSE:
            return publiclobbies;
        case CONCERTO_LOBBY:
            return lobbyentries;
        default:
            return {};
    }
}

vector<string> SteamLobby::getIps()
{
    if ( mode == CONCERTO_LOBBY )
        return lobbyips;
    return {};
}

vector<string> SteamLobby::getIds()
{
    return lobbyids;
}


// ---- browse / create / join ----

void SteamLobby::fetchPublicLobby()
{
    SteamManager::get().requestPublicLobbies();
    _pending = P_BROWSE;
}

void SteamLobby::create ( string name, string type )
{
    SteamManager::get().setLobbyMemberName ( name );
    SteamManager::get().createLobby ( type == "Public" );
    _pending = P_CREATE;
}

string SteamLobby::join ( string name, int selection )
{
    SteamManager& sm = SteamManager::get();
    const vector<SteamManager::LobbyInfo>& lobbies = sm.publicLobbies();

    string code;
    if ( selection >= 0 && selection < ( int ) lobbies.size() )
    {
        sm.setLobbyMemberName ( name );
        sm.joinLobbyById ( lobbies[selection].lobbyId );
        code = lobbies[selection].code;
        _pending = P_JOIN;
    }
    return code;
}

void SteamLobby::join ( string name, string code )
{
    SteamManager::get().setLobbyMemberName ( name );
    SteamManager::get().joinLobbyByCode ( code );
    _pending = P_JOIN;
}

bool SteamLobby::checkLobbyCode ( string code )
{
    if ( code.size() < 2 || code.size() > 8 )
        return false;
    for ( size_t i = 0; i < code.size(); ++i )
        if ( ! isalnum ( ( unsigned char ) code[i] ) )
            return false;
    return true;
}


// ---- challenge handshake ----

void SteamLobby::challenge ( string target, IpAddrPort port )
{
    SteamManager& sm = SteamManager::get();
    const uint64_t targetId = strtoull ( target.c_str(), nullptr, 10 );
    const uint64_t myId = sm.getSteamID();

    // Advertise the challenge (also publishes our SteamID as host_id).
    sm.setChallenge ( targetId );

    // Let the peer's member-data arrive, then re-read for the simultaneous-challenge tie-break.
    for ( int i = 0; i < 40; ++i )
    {
        sm.pump();
        Sleep ( 5 );
    }

    bool peerChallengingMe = false;
    for ( const SteamManager::MemberInfo& m : sm.lobbyMembers() )
        if ( m.steamId == targetId && m.challengingTarget == myId )
            peerChallengingMe = true;

    // Tie-break: if we both challenged each other, the LOWER SteamID hosts. The higher SteamID
    // backs off (clears its challenge) and reconnects as Client - the peer's host row appears on
    // the next refresh, so selecting it again takes the (now non-"None") client branch.
    if ( peerChallengingMe && myId > targetId )
    {
        sm.clearChallenge();
        hostSuccess = false;
        refresh();
        return;
    }

    hostSuccess = true;
}

void SteamLobby::preaccept ( string id )
{
    // Client side: nothing to negotiate over Steam - the caller connects directly to the host's
    // SteamID (already resolved into the address by MainUi from the member's host row).
}

void SteamLobby::accept()
{
    // Server-backed lobby notifies the relay that the connection is up; over Steam the P2P
    // connection is direct, so there is nothing to confirm.
}

void SteamLobby::unhost()
{
    SteamManager::get().clearChallenge();
    hostSuccess = false;
}

void SteamLobby::end()
{
    // Stay in the lobby; just make sure our challenge advertisement is withdrawn.
    SteamManager::get().clearChallenge();
}

void SteamLobby::host ( string name, IpAddrPort port )
{
    // DEFAULT_LOBBY (relay-only peer list) has no Steam analog and is not reachable from the
    // Steam menu (getMenu() omits it). No-op.
}


// ---- backend seam ----

void SteamLobby::refresh()
{
    SteamManager& sm = SteamManager::get();

    // Process any pending lobby-data / chat callbacks so the member cache is current.
    for ( int i = 0; i < 3; ++i )
        sm.pump();

    LOCK ( entryMutex );

    if ( mode == CONCERTO_LOBBY )
    {
        const uint64_t myId = sm.getSteamID();
        const vector<SteamManager::MemberInfo> members = sm.lobbyMembers();

        lobbyentries.clear();
        lobbyips.clear();
        lobbyids.clear();

        for ( const SteamManager::MemberInfo& m : members )
        {
            if ( m.steamId == myId )
                continue; // don't list / challenge ourselves

            char idStr[32];
            snprintf ( idStr, sizeof ( idStr ), "%llu", ( unsigned long long ) m.steamId );

            string disp = m.name.empty() ? string ( idStr ) : m.name;
            string ip = "None";

            if ( m.challengingTarget == myId )
            {
                // This member is challenging us and is hosting -> we connect to them as Client.
                ip = steamAddr ( m.hostId ? m.hostId : m.steamId );
            }
            else if ( m.challengingTarget != 0 )
            {
                disp += " (busy)";
            }

            lobbyentries.push_back ( disp );
            lobbyids.push_back ( idStr );
            lobbyips.push_back ( ip );
        }

        numEntries = ( int ) lobbyentries.size();
    }
    else if ( mode == CONCERTO_BROWSE )
    {
        publiclobbies.clear();
        roomcodes.clear();

        for ( const SteamManager::LobbyInfo& li : sm.publicLobbies() )
        {
            roomcodes.push_back ( li.code );
            publiclobbies.push_back ( li.code + "   " + to_string ( li.memberCount ) );
        }

        numEntries = ( int ) publiclobbies.size();
    }
}

void SteamLobby::pumpUntilSettled()
{
    SteamManager& sm = SteamManager::get();

    if ( _pending == P_NONE )
        return;

    if ( _pending == P_BROWSE )
    {
        for ( int i = 0; i < SETTLE_TRIES && sm.browseState() == SteamManager::LobbyState::Working; ++i )
        {
            sm.pump();
            Sleep ( SETTLE_SLEEP_MS );
        }
    }
    else
    {
        for ( int i = 0; i < SETTLE_TRIES && sm.lobbyState() == SteamManager::LobbyState::Working; ++i )
        {
            sm.pump();
            Sleep ( SETTLE_SLEEP_MS );
        }
    }

    finishPending();
}

void SteamLobby::finishPending()
{
    SteamManager& sm = SteamManager::get();
    const Pending p = _pending;
    _pending = P_NONE;

    switch ( p )
    {
        case P_CREATE:
        case P_JOIN:
            if ( sm.lobbyState() == SteamManager::LobbyState::Ready )
            {
                mode = CONCERTO_LOBBY;
                lobbyMsg = sm.lobbyCode(); // host's code (empty for a joiner, which is fine)
                refresh();
            }
            else
            {
                lobbyError = "Failed";
            }
            break;

        case P_BROWSE:
            if ( sm.browseState() == SteamManager::LobbyState::Ready )
                refresh();
            break;

        default:
            break;
    }
}

#endif // ENABLE_STEAM
