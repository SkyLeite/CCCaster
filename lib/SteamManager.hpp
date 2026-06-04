#pragma once

#ifdef ENABLE_STEAM

#include "Timer.hpp"
#include "Protocol.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>


class SteamSocket;


// Owns the Steamworks API lifecycle and is the ONLY translation unit that includes the
// Steam SDK headers. Everything Steam-related is wrapped behind plain-typed functions
// (uint32_t connection/listen handles, uint64_t SteamIDs) so the rest of CCCaster never
// pulls in Steam headers. Calls that pass/return CSteamID use the flat C API to avoid the
// MinGW<->MSVC ABI mismatch (see the steamworks-mingw-flat-api note).
//
// A self-driven repeating Timer pumps Steam's manual callback dispatch + drains incoming
// messages, so SteamSocket does not need to register with SocketManager (which select()s on an fd).
class SteamManager : private Timer::Owner
{
public:

    static SteamManager& get();

    // Ref-counted SteamAPI init. ref() returns false if Steam is unavailable (not running,
    // app 411370 not owned, or no steam_appid.txt). Safe to call repeatedly.
    bool ref();
    void deref();

    bool isInitialized() const { return _inited; }

    // Local user's SteamID64 (0 if not initialized).
    uint64_t getSteamID() const;

    // Whether the SDR relay network is ready (ping data current). ConnectP2P before this is
    // ready tends to fail, so callers should gate on it.
    bool isRelayNetworkReady() const;

    // ---- transport wrappers (all handles are plain ints; 0 == invalid) ----

    // Host: open a P2P listen socket on a virtual port. Returns the listen handle.
    uint32_t createListenSocketP2P ( int virtualPort );
    void closeListenSocket ( uint32_t listenHandle );

    // Client: begin a P2P connection to a peer SteamID on a virtual port. Returns conn handle.
    uint32_t connectP2P ( uint64_t peerSteamId, int virtualPort );

    bool acceptConnection ( uint32_t conn );
    void closeConnection ( uint32_t conn );

    // Send already-encoded bytes over a connection. reliable picks the Steam send flag.
    bool sendMessage ( uint32_t conn, const void *data, size_t len, bool reliable );

    // Peer SteamID for an established connection (0 if unknown).
    uint64_t getConnectionPeer ( uint32_t conn ) const;

    // ---- lobby / matchmaking (driven by the launcher UI) ----
    // Asynchronous; poll lobbyState() until Ready/Failed. SteamManager's pump runs the
    // underlying Steam callbacks, so the caller just needs to keep pumping the event loop.
    enum class LobbyState { Idle, Working, Ready, Failed };

    // Host: create a searchable lobby and advertise a short join code + our SteamID.
    // On Ready, lobbyCode() returns the code to share.
    void hostLobby();

    // Client: find the lobby advertising the given code and resolve the host's SteamID.
    // On Ready, lobbyPeerId() returns the host SteamID64 to ConnectP2P to.
    void joinLobbyByCode ( const std::string& code );

    void leaveLobby();

    LobbyState lobbyState() const { return _lobbyState; }
    const std::string& lobbyCode() const { return _lobbyCode; }
    uint64_t lobbyPeerId() const { return _lobbyPeerId; }

    // Run one manual-dispatch + message-drain cycle. Normally driven by the internal pump
    // timer once the EventManager loop is running, but callers (e.g. the lobby UI) can call
    // it directly in a wait loop when no EventManager loop is pumping yet.
    void pump();

    // Called by the internal Steam callback shim (defined in the .cpp).
    void onLobbyCreatedResult ( bool ok, uint64_t lobbyId );
    void onLobbyListResult ( bool found, uint64_t lobbyId );

    // ---- routing registries (used by SteamSocket) ----

    void registerConnection ( uint32_t conn, SteamSocket *socket );
    void unregisterConnection ( uint32_t conn );

    void registerListen ( uint32_t listenHandle, SteamSocket *server );
    void unregisterListen ( uint32_t listenHandle );

    // Called by the internal Steam callback shim (defined in the .cpp).
    void onConnectionStatusChanged ( uint32_t conn, uint32_t listenHandle, int newState, uint64_t peerSteamId );

private:

    SteamManager() {}

    int _refCount = 0;
    bool _inited = false;

    // Steam pipe for manual callback dispatch (HSteamPipe; 0 == none).
    int _pipe = 0;

    // Pending async call-result ids (SteamAPICall_t) matched in the manual-dispatch pump.
    uint64_t _lobbyCreateCall = 0;
    uint64_t _lobbyListCall = 0;

    TimerPtr _pumpTimer;

    // Lobby state
    LobbyState _lobbyState = LobbyState::Idle;
    std::string _lobbyCode;     // host's advertised code (valid when host + Ready)
    std::string _joinCode;      // code the client is searching for
    uint64_t _lobbyId = 0;      // current lobby (host or joined)
    uint64_t _lobbyPeerId = 0;  // resolved host SteamID (valid when client + Ready)
    bool _lobbyIsHost = false;

    // conn handle -> the SteamSocket that owns it (client or accepted child)
    std::unordered_map<uint32_t, SteamSocket *> _connections;

    // listen handle -> the server SteamSocket that owns it
    std::unordered_map<uint32_t, SteamSocket *> _listeners;

    void startPump();
    void stopPump();

    void timerExpired ( Timer *timer ) override;
};

#endif // ENABLE_STEAM
