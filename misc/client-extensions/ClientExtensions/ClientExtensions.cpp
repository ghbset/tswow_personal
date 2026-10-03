#include "ClientExtensions.h"

void InstallSpellQueue();   // Gameplay/SpellQueue.cpp
void InstallSmoothTurn();   // Gameplay/SmoothTurn.cpp

void ClientExtensions::initialize() {
    CharacterFixes::CharacterCreationFixes();
    SpellTooltipExtensions::Apply();
    InstallSpellQueue();
    InstallSmoothTurn();
}
