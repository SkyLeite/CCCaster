#include "Thread.hpp"
#include "Logger.hpp"
#include "Steam.hpp"

#include <steam/steam_api.h>
#include <thread>
#include <chrono>

void SteamThread::run()
{
    LOG("Running Steam callbacks thread");
    while (true) {
        SteamAPI_RunCallbacks();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
