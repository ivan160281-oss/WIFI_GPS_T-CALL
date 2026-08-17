#pragma once
// Modem model + board pin definitions - kept in a separate header, included
// FIRST (before <TinyGsmClient.h>), rather than as #define lines directly in
// the .ino. This isn't just style: the Arduino builder's automatic function-
// prototype generation can insert generated prototypes between top-level
// #define and #include lines in a .ino file, which silently breaks the
// "#define TINY_GSM_MODEM_A7670 must come before #include <TinyGsmClient.h>"
// requirement even though the source reads top-to-bottom correctly. Pulling
// the defines into their own header (a plain .h file isn't subject to that
// prototype-insertion pass) sidesteps it entirely - this is the same pattern
// LilyGO's own T-A76XX example sketches use, not a guess.
//
// Board: LILYGO T-Call A7670 V1.0/V1.1. Pin numbers sourced from LilyGO's own
// reference firmware (LilyGO-T-A76XX/examples/*/utilities.h) - double-check
// against your specific board revision's silkscreen/schematic before
// flashing a whole batch.

// The mainline TinyGSM library (installed via Arduino's Library Manager -
// see .github/workflows/build.yml) has NO driver named "A7670" - confirmed
// against its own TinyGsmClient.h, which lists every #ifdef it actually
// checks. LilyGO's own official example for this exact board
// (LilyGo-T-PCIE/examples/A7670/TinyGSM_Net_GNSS) uses SIM7600 compatibility
// mode instead, with the comment "A7670 Compatible with SIM7600 AT
// instructions" - that's not a guess on our part, it's LilyGO's own choice
// for the same hardware, and it's what actually exists in the library this
// firmware pulls in. (TINY_GSM_MODEM_A7670 does exist in a separate LilyGO
// fork of TinyGSM with dedicated A7670 support, but the standard Library
// Manager install - what anyone building this from the given workflow gets -
// is the upstream vshymanskyy/TinyGSM, which doesn't have it.)
#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_RX_BUFFER   1024

#define MODEM_BAUDRATE       115200
#define MODEM_TX_PIN         26
#define MODEM_RX_PIN         25
#define MODEM_DTR_PIN        14
#define MODEM_RESET_PIN      27
#define MODEM_RESET_LEVEL    LOW
#define BOARD_PWRKEY_PIN     4
#define BOARD_LED_PIN        12
#define LED_ON                HIGH
#define SerialAT             Serial1
