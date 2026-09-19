// Плата и режим сборки для Arduino IDE. В PlatformIO этого файла нет —
// там то же самое лежит в build_flags (среда yasmina).
#pragma once

#define BOARD_S3_YASMINA   // распиновка спаянной платы, см. config.h
#define BOARD_HAS_PSRAM    // N16R8, октальная PSRAM
#define VOICE_ENABLED      // голосовой диалог; убери — останется демо лица
