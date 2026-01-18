#include "Thread.hpp"
#include <steam/steam_api.h>

class SteamThread : public Thread {
    private:    
        Mutex mutex;
        bool running = false;

    public:
        void run() override;
};

inline static SteamThread steamThread;