#ifdef ENABLE_STEAM

#include "SteamManager.hpp"
#include "SteamSocket.hpp"
#include "Logger.hpp"

#include "steam/steam_api.h"
// Flat C API for ALL Steam interface calls. On 32-bit MinGW the C++ interface vtable +
// auto-callback (STEAM_CALLBACK/CCallResult) ABI does not match the MSVC-built steam_api.dll
// The flat API is plain extern "C", and we use MANUAL callback dispatch below instead of
// letting the DLL call into our C++ vtables.
// See the steamworks-mingw-flat-api note.
#include "steam/steam_api_flat.h"

#include <vector>
#include <cstdlib>
#include <cstdio>
#include <ctime>

using namespace std;


// How often to pump dispatch + drain messages
#define PUMP_INTERVAL_MS ( 2 )

// Max messages drained per connection per pump (leftovers come next pump).
#define RECV_BATCH ( 32 )

// Lobby metadata keys: the shareable join code, and the host's SteamID64
static const char *LOBBY_CODE_KEY = "cccaster_code";
static const char *LOBBY_HOST_KEY = "host_id";

// Route Steam diagnostics into our log file
static void S_CALLTYPE steamNetDebugOutput ( ESteamNetworkingSocketsDebugOutputType type, const char *msg )
{
    LOG ( "[SteamNet] %s", msg ? msg : "" );
}

static void S_CALLTYPE steamWarningHook ( int severity, const char *msg )
{
    LOG ( "[SteamWarn:%d] %s", severity, msg ? msg : "" );
}

// Human-shareable join code
static string makeLobbyCode()
{
    static bool seeded = false;
    if ( ! seeded )
    {
        srand ( ( unsigned ) time ( nullptr ) );
        seeded = true;
    }
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    string s;
    for ( int i = 0; i < 6; ++i )
        s += alphabet[rand() % ( sizeof ( alphabet ) - 1 )];
    return s;
}


SteamManager& SteamManager::get()
{
    static SteamManager instance;
    return instance;
}

bool SteamManager::ref()
{
    if ( _refCount > 0 )
    {
        ++_refCount;
        return _inited;
    }

    if ( ! SteamAPI_Init() )
    {
        LOG ( "SteamAPI_Init() failed (Steam not running, app not owned, or no steam_appid.txt)" );
        return false;
    }

    _inited = true;
    _refCount = 1;

    // poll callbacks from POD structs in pump() rather than
    // having steam_api.dll call back into us
    SteamAPI_ManualDispatch_Init();
    _pipe = SteamAPI_GetHSteamPipe();

    // Redirect Steam's stdout into our log
    SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction (
        SteamNetworkingUtils(), k_ESteamNetworkingSocketsDebugOutputType_Warning, steamNetDebugOutput );
    SteamAPI_ISteamUtils_SetWarningMessageHook ( SteamUtils(), steamWarningHook );

    // Disable ECN for extra Wine compatibility
    SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 (
        SteamNetworkingUtils(), k_ESteamNetworkingConfig_ECN, 0 );

    SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess ( SteamNetworkingUtils() );

    LOG ( "Steam initialized; user=%llu", ( unsigned long long ) getSteamID() );

    startPump();
    return true;
}

void SteamManager::deref()
{
    if ( _refCount <= 0 )
        return;

    --_refCount;

    if ( _refCount > 0 )
        return;

    stopPump();

    SteamAPI_Shutdown();
    _inited = false;
    _pipe = 0;
    _lobbyCreateCall = _lobbyListCall = 0;

    LOG ( "Steam shut down" );
}

uint64_t SteamManager::getSteamID() const
{
    if ( ! _inited )
        return 0;

    return SteamAPI_ISteamUser_GetSteamID ( SteamUser() );
}

bool SteamManager::isRelayNetworkReady() const
{
    if ( ! _inited )
        return false;

    return ( SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus ( SteamNetworkingUtils(), nullptr )
             == k_ESteamNetworkingAvailability_Current );
}


// ---- transport wrappers ----

uint32_t SteamManager::createListenSocketP2P ( int virtualPort )
{
    if ( ! _inited )
        return 0;

    return SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P (
               SteamNetworkingSockets(), virtualPort, 0, nullptr );
}

void SteamManager::closeListenSocket ( uint32_t listenHandle )
{
    if ( _inited && listenHandle )
        SteamAPI_ISteamNetworkingSockets_CloseListenSocket ( SteamNetworkingSockets(), listenHandle );
}

uint32_t SteamManager::connectP2P ( uint64_t peerSteamId, int virtualPort )
{
    if ( ! _inited )
        return 0;

    SteamNetworkingIdentity identity;
    identity.SetSteamID64 ( peerSteamId ); // inline, no DLL call

    return SteamAPI_ISteamNetworkingSockets_ConnectP2P (
               SteamNetworkingSockets(), identity, virtualPort, 0, nullptr );
}

bool SteamManager::acceptConnection ( uint32_t conn )
{
    if ( ! _inited || ! conn )
        return false;

    return ( SteamAPI_ISteamNetworkingSockets_AcceptConnection ( SteamNetworkingSockets(), conn )
             == k_EResultOK );
}

void SteamManager::closeConnection ( uint32_t conn )
{
    if ( _inited && conn )
        SteamAPI_ISteamNetworkingSockets_CloseConnection ( SteamNetworkingSockets(), conn, 0, nullptr, false );
}

bool SteamManager::sendMessage ( uint32_t conn, const void *data, size_t len, bool reliable )
{
    if ( ! _inited || ! conn )
        return false;

    const int flags = ( reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable );

    const EResult r = SteamAPI_ISteamNetworkingSockets_SendMessageToConnection (
                          SteamNetworkingSockets(), conn, data, ( uint32 ) len, flags, nullptr );

    return ( r == k_EResultOK );
}

uint64_t SteamManager::getConnectionPeer ( uint32_t conn ) const
{
    if ( ! _inited || ! conn )
        return 0;

    SteamNetConnectionInfo_t info;
    if ( ! SteamAPI_ISteamNetworkingSockets_GetConnectionInfo ( SteamNetworkingSockets(), conn, &info ) )
        return 0;

    return info.m_identityRemote.GetSteamID64(); // inline accessor
}


// ---- registries ----

void SteamManager::registerConnection ( uint32_t conn, SteamSocket *socket )
{
    if ( conn )
        _connections[conn] = socket;
}

void SteamManager::unregisterConnection ( uint32_t conn )
{
    _connections.erase ( conn );
}

void SteamManager::registerListen ( uint32_t listenHandle, SteamSocket *server )
{
    if ( listenHandle )
        _listeners[listenHandle] = server;
}

void SteamManager::unregisterListen ( uint32_t listenHandle )
{
    _listeners.erase ( listenHandle );
}


// ---- lobby / matchmaking ----

void SteamManager::hostLobby()
{
    if ( ! _inited )
    {
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyIsHost = true;
    _lobbyState = LobbyState::Working;
    _lobbyCode.clear();
    _lobbyId = 0;

    // Result matched by id in pump()
    _lobbyCreateCall = SteamAPI_ISteamMatchmaking_CreateLobby ( SteamMatchmaking(), k_ELobbyTypePublic, 2 );
}

void SteamManager::onLobbyCreatedResult ( bool ok, uint64_t lobbyId )
{
    if ( ! ok )
    {
        LOG ( "CreateLobby failed" );
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyId = lobbyId;

    if ( _lobbyCode.empty() )
        _lobbyCode = makeLobbyCode();

    char hostId[32];
    snprintf ( hostId, sizeof ( hostId ), "%llu", ( unsigned long long ) getSteamID() );

    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_CODE_KEY, _lobbyCode.c_str() );
    SteamAPI_ISteamMatchmaking_SetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_HOST_KEY, hostId );

    _lobbyState = LobbyState::Ready;
    LOG ( "Lobby ready; code=%s", _lobbyCode.c_str() );
}

void SteamManager::joinLobbyByCode ( const std::string& code )
{
    if ( ! _inited )
    {
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyIsHost = false;
    _joinCode = code;
    _lobbyPeerId = 0;
    _lobbyState = LobbyState::Working;

    SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter (
        SteamMatchmaking(), LOBBY_CODE_KEY, code.c_str(), k_ELobbyComparisonEqual );
    SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter (
        SteamMatchmaking(), k_ELobbyDistanceFilterWorldwide );

    _lobbyListCall = SteamAPI_ISteamMatchmaking_RequestLobbyList ( SteamMatchmaking() );
}

void SteamManager::onLobbyListResult ( bool found, uint64_t lobbyId )
{
    if ( ! found )
    {
        LOG ( "No lobby found for code '%s'", _joinCode.c_str() );
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyId = lobbyId;

    const char *hostId = SteamAPI_ISteamMatchmaking_GetLobbyData ( SteamMatchmaking(), lobbyId, LOBBY_HOST_KEY );
    _lobbyPeerId = ( hostId && hostId[0] ) ? strtoull ( hostId, nullptr, 10 ) : 0;

    if ( _lobbyPeerId == 0 )
    {
        LOG ( "Lobby found but host_id missing" );
        _lobbyState = LobbyState::Failed;
        return;
    }

    _lobbyState = LobbyState::Ready;
    LOG ( "Joined lobby; host=%llu", ( unsigned long long ) _lobbyPeerId );
}

void SteamManager::leaveLobby()
{
    if ( _inited && _lobbyId )
        SteamAPI_ISteamMatchmaking_LeaveLobby ( SteamMatchmaking(), _lobbyId );

    _lobbyId = 0;
    _lobbyPeerId = 0;
    _lobbyCode.clear();
    _lobbyState = LobbyState::Idle;
}


// ---- callback routing ----

void SteamManager::onConnectionStatusChanged ( uint32_t conn, uint32_t listenHandle,
                                               int newState, uint64_t peerSteamId )
{
    switch ( newState )
    {
        case k_ESteamNetworkingConnectionState_Connecting:
        {
            if ( listenHandle )
            {
                const auto it = _listeners.find ( listenHandle );
                if ( it != _listeners.end() )
                    it->second->onIncomingConnection ( conn, peerSteamId );
                else
                    closeConnection ( conn );
            }
            break;
        }

        case k_ESteamNetworkingConnectionState_Connected:
        {
            const auto it = _connections.find ( conn );
            if ( it != _connections.end() )
                it->second->onConnected();
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            const auto it = _connections.find ( conn );
            if ( it != _connections.end() )
                it->second->onClosed();
            else
                closeConnection ( conn );
            break;
        }

        default:
            break;
    }
}


// ---- pump (manual dispatch) ----

void SteamManager::startPump()
{
    if ( ! _pumpTimer )
        _pumpTimer.reset ( new Timer ( this ) );
    _pumpTimer->start ( PUMP_INTERVAL_MS );
}

void SteamManager::stopPump()
{
    _pumpTimer.reset();
}

void SteamManager::timerExpired ( Timer *timer )
{
    if ( timer != _pumpTimer.get() )
        return;

    pump();

    if ( _pumpTimer )
        _pumpTimer->start ( PUMP_INTERVAL_MS );
}

void SteamManager::pump()
{
    if ( ! _inited )
        return;

    // --- manual callback dispatch ---
    SteamAPI_ManualDispatch_RunFrame ( _pipe );

    CallbackMsg_t cb;
    while ( SteamAPI_ManualDispatch_GetNextCallback ( _pipe, &cb ) )
    {
        if ( cb.m_iCallback == SteamAPICallCompleted_t::k_iCallback )
        {
            // Async call result (CreateLobby / RequestLobbyList)
            const SteamAPICallCompleted_t *cc = ( const SteamAPICallCompleted_t * ) cb.m_pubParam;
            void *result = malloc ( cc->m_cubParam );
            bool failed = false;

            if ( result && SteamAPI_ManualDispatch_GetAPICallResult (
                     _pipe, cc->m_hAsyncCall, result, cc->m_cubParam, cc->m_iCallback, &failed ) )
            {
                if ( _lobbyCreateCall && cc->m_hAsyncCall == _lobbyCreateCall )
                {
                    const LobbyCreated_t *r = ( const LobbyCreated_t * ) result;
                    _lobbyCreateCall = 0;
                    onLobbyCreatedResult ( ( ! failed && r->m_eResult == k_EResultOK ), r->m_ulSteamIDLobby );
                }
                else if ( _lobbyListCall && cc->m_hAsyncCall == _lobbyListCall )
                {
                    const LobbyMatchList_t *r = ( const LobbyMatchList_t * ) result;
                    _lobbyListCall = 0;
                    const bool found = ( ! failed && r->m_nLobbiesMatching > 0 );
                    const uint64 lobbyId =
                        found ? SteamAPI_ISteamMatchmaking_GetLobbyByIndex ( SteamMatchmaking(), 0 ) : 0;
                    onLobbyListResult ( found, lobbyId );
                }
            }

            free ( result );
        }
        else if ( cb.m_iCallback == SteamNetConnectionStatusChangedCallback_t::k_iCallback )
        {
            const SteamNetConnectionStatusChangedCallback_t *p =
                ( const SteamNetConnectionStatusChangedCallback_t * ) cb.m_pubParam;
            onConnectionStatusChanged ( p->m_hConn, p->m_info.m_hListenSocket,
                                        ( int ) p->m_info.m_eState,
                                        p->m_info.m_identityRemote.GetSteamID64() );
        }

        SteamAPI_ManualDispatch_FreeLastCallback ( _pipe );
    }

    // --- drain inbound messages ---
    vector<uint32_t> conns;
    conns.reserve ( _connections.size() );
    for ( const auto& kv : _connections )
        conns.push_back ( kv.first );

    for ( uint32_t conn : conns )
    {
        if ( _connections.find ( conn ) == _connections.end() )
            continue;

        SteamNetworkingMessage_t *msgs[RECV_BATCH];
        const int n = SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection (
                          SteamNetworkingSockets(), conn, msgs, RECV_BATCH );

        vector<MsgPtr> decoded;
        decoded.reserve ( n > 0 ? n : 0 );
        for ( int i = 0; i < n; ++i )
        {
            size_t consumed = 0;
            MsgPtr msg = ::Protocol::decode ( ( const char * ) msgs[i]->m_pData, msgs[i]->m_cbSize, consumed );
            msgs[i]->Release(); // frees via the message's own fn ptr
            if ( msg.get() )
                decoded.push_back ( msg );
        }

        for ( const MsgPtr& msg : decoded )
        {
            const auto it = _connections.find ( conn );
            if ( it == _connections.end() )
                break;
            it->second->onReceive ( msg );
        }
    }
}

#endif // ENABLE_STEAM
