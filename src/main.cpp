#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace {
    struct Pending {
        int ticks = 0;          // ticks que quedan de memoria (0 = nada guardado)
        bool released = false;  // el jugador ya solto el boton
        bool releaseNext = false; // falta soltar el click que repetimos
        bool isPlayer1 = true;
    };

    Pending s_pending[2];
    bool s_usedAssist = false;
    bool s_oldTestMode = false;

    void clearPending() {
        s_pending[0] = Pending{};
        s_pending[1] = Pending{};
    }

    // Modos que necesitan suelo u orbe (cubo, bola, robot, arana)
    bool needsSurface(PlayerObject* p) {
        return !p->m_isShip && !p->m_isBird && !p->m_isDart && !p->m_isSwing;
    }

    bool canActNow(PlayerObject* p) {
        return p->m_isOnGround || (p->m_touchingRings && p->m_touchingRings->count() > 0);
    }
}

class $modify(ClickFixLayer, GJBaseGameLayer) {
    // Solo en niveles jugados, no en el editor
    bool isActive() {
        return Mod::get()->getSettingValue<bool>("enabled")
            && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this;
    }

    PlayerObject* playerFor(int slot) {
        return slot == 1 ? m_player2 : m_player1;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // button 1 = salto
        if (button == 1 && this->isActive()) {
            bool twoPlayers = m_levelSettings && m_levelSettings->m_twoPlayerMode;
            int slot = (twoPlayers && !isPlayer1) ? 1 : 0;
            auto player = this->playerFor(slot);
            auto& pending = s_pending[slot];

            if (down) {
                pending = Pending{};
                if (player && needsSurface(player) && !player->m_isDead && !canActNow(player)) {
                    pending.ticks = static_cast<int>(
                        Mod::get()->getSettingValue<int64_t>("window-ticks"));
                    pending.isPlayer1 = isPlayer1;
                }
            }
            else if (pending.ticks > 0) {
                pending.released = true;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (this->isActive()) {
            for (int slot = 0; slot < 2; slot++) {
                auto& pending = s_pending[slot];
                auto player = this->playerFor(slot);
                if (!player) continue;

                if (pending.releaseNext) {
                    if (!isHalfTick) {
                        GJBaseGameLayer::handleButton(false, 1, pending.isPlayer1);
                        pending = Pending{};
                    }
                    continue;
                }
                if (pending.ticks <= 0) continue;

                if (player->m_isDead || !needsSurface(player)) {
                    pending = Pending{};
                }
                else if (canActNow(player)) {
                    if (pending.released) {
                        // El click llego antes de tiempo: se repite ahora
                        s_usedAssist = true;
                        PlayLayer::get()->m_isTestMode = true;
                        GJBaseGameLayer::handleButton(true, 1, pending.isPlayer1);
                        pending.ticks = 0;
                        pending.releaseNext = true;
                    }
                    else {
                        // Sigue pulsado: el juego ya lo resuelve solo
                        pending = Pending{};
                    }
                }
                else if (!isHalfTick) {
                    pending.ticks--;
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(ClickFixPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        clearPending();
        s_usedAssist = false;
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        s_oldTestMode = m_isTestMode;
        return true;
    }

    void resetLevel() {
        clearPending();
        // Un intento nuevo empieza limpio
        if (s_usedAssist) {
            m_isTestMode = s_oldTestMode;
            s_usedAssist = false;
        }
        PlayLayer::resetLevel();
    }
};
