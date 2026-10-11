#pragma once

namespace Csis {

    enum Type_heat_level {
        Type_heat_level_2  = 1,
        Type_heat_level_3  = 2,
        Type_heat_level_4  = 4,
        Type_heat_level_5  = 8,
        Type_heat_level_6  = 16,
        Type_heat_level_7  = 32,
        Type_heat_level_8  = 64,
        Type_heat_level_9  = 128,
        Type_heat_level_10 = 256,
    };

}

class SoundAI {
  public:
    struct HeatCutoffs {
        float                 value;
        Csis::Type_heat_level heat_level;
    };

    static bool InstallPatches(void* module);
    static void RestorePatches();

    void SyncPlayers();
    void SyncPlayersAndRearmHeatJump();

    unsigned char mBeforePlayerHeat[0x100];
    int           mPlayerHeat;
};
