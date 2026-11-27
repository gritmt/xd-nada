#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

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

    // Estado de la ayuda para wave y nave
    struct WaveState {
        bool held = false;      // lo que el juego cree que esta pulsado
        bool deferred = false;  // hay una pulsacion o soltada retrasada
        bool deferredDown = false;
        bool isPlayer1 = true;
        int wait = 0;           // ticks que faltan para aplicarla
        int heldFor = 0;        // ticks desde la ultima pulsacion
        bool hasX = false;
        float lastX = 0.f;
        float step = 0.f;       // avance horizontal por tick
        // Solo nave
        bool hasY = false;
        bool hasVy = false;
        bool prevHeld = false;
        bool mini = false;
        float lastY = 0.f;
        float vy = 0.f;         // positivo = hacia donde empuja el boton
        int sincePress = 1000;  // ticks desde la ultima pulsacion
        int taps = 0;           // pulsaciones seguidas
    };

    WaveState s_wave[2];

    // Ajustes en memoria, para no buscarlos por nombre en cada tick
    struct Settings {
        bool enabled = true, wavePredict = true, shipPredict = true, shipStraight = true;
        int windowTicks = 12, waveMinTap = 4, waveMaxDelay = 3;
        int shipMinTap = 3, shipMaxDelay = 4, straightStrength = 40;

        void load() {
            auto mod = Mod::get();
            auto flag = [&](char const* key) { return mod->getSettingValue<bool>(key); };
            auto number = [&](char const* key) {
                return static_cast<int>(mod->getSettingValue<int64_t>(key));
            };
            enabled = flag("enabled");
            wavePredict = flag("wave-predict");
            shipPredict = flag("ship-predict");
            shipStraight = flag("ship-straight");
            windowTicks = number("window-ticks");
            waveMinTap = number("wave-min-tap");
            waveMaxDelay = number("wave-max-delay");
            shipMinTap = number("ship-min-tap");
            shipMaxDelay = number("ship-max-delay");
            straightStrength = number("ship-straight-strength");
        }
    };

    Settings s_cfg;
    int s_reloadIn = 0;

    // Solo bloques y pinchos del nivel, para no recorrer la decoracion
    struct ObjectIndex {
        std::vector<std::pair<float, GameObject*>> fixed; // sin grupos, ordenados por X
        std::vector<GameObject*> movable;                 // con grupos: pueden moverse
        int indexed = -1;                                 // objetos que tenia el nivel
    };

    ObjectIndex s_index;

    // Como acelera la nave, medido mientras se juega
    struct ShipModel {
        float accel[2][2] = {};  // [pulsado][subiendo]
        int count[2][2] = {};
        float cap[2] = {100.f, 100.f}; // velocidad maxima [subiendo]

        void learn(bool held, bool rising, float a) {
            int& n = count[held][rising];
            if (n < 60) n++;
            accel[held][rising] += (a - accel[held][rising]) / n;
        }
        bool known(bool held) const {
            return count[held][0] >= 3 || count[held][1] >= 3;
        }
        float get(bool held, bool rising) const {
            return accel[held][count[held][rising] >= 3 ? rising : !rising];
        }
    };

    ShipModel s_ship[2]; // normal y mini

    // Ticks que se miran hacia delante con la nave
    constexpr int SHIP_LOOKAHEAD = 40;
    // Maximo de ticks entre toques para contar como spam
    constexpr int SPAM_GAP = 48;

    bool isFlyer(PlayerObject* p) {
        return p->m_isDart || p->m_isShip;
    }

    bool isStraight(PlayerObject* p, WaveState const& w) {
        return p->m_isShip && !p->m_isPlatformer && !p->m_isDashing
            && w.taps >= 3 && w.sincePress <= SPAM_GAP
            && s_cfg.shipStraight;
    }

    // Ticks que se miran hacia delante
    constexpr int LOOKAHEAD = 16;

    struct HitObstacle {
        CCRect rect;
        float radius; // mayor que 0 en objetos redondos (sierras)
        bool solid = false;
    };

    bool touches(CCRect const& player, HitObstacle const& o) {
        if (o.radius <= 0.f) return player.intersectsRect(o.rect);
        float cx = o.rect.getMidX();
        float cy = o.rect.getMidY();
        float dx = cx - std::clamp(cx, player.getMinX(), player.getMaxX());
        float dy = cy - std::clamp(cy, player.getMinY(), player.getMaxY());
        return dx * dx + dy * dy <= o.radius * o.radius;
    }

    // Primer tick en el que habria choque si el boton cambia tras "delay" ticks.
    // Devuelve un numero mayor que "horizon" si no hay choque.
    int firstHit(
        CCRect box, std::vector<HitObstacle> const& obstacles,
        float dx, float dy, bool upsideDown,
        bool heldNow, bool newState, int delay, int horizon
    ) {
        for (int t = 1; t <= horizon; t++) {
            bool held = t <= delay ? heldNow : newState;
            bool goesUp = held != upsideDown;
            box.origin.x += dx;
            box.origin.y += goesUp ? dy : -dy;
            for (auto const& o : obstacles) {
                if (touches(box, o)) return t;
            }
        }
        return horizon + 1;
    }

    // Igual que firstHit, pero con la inercia de la nave
    int firstShipHit(
        CCRect box, std::vector<HitObstacle> const& obstacles,
        ShipModel const& model, float dx, float vy, bool upsideDown,
        bool heldNow, bool newState, int delay, int horizon
    ) {
        float gravity = upsideDown ? -1.f : 1.f;
        for (int t = 1; t <= horizon; t++) {
            bool held = t <= delay ? heldNow : newState;
            vy += model.get(held, vy > 0.f);
            float cap = model.cap[vy > 0.f];
            vy = std::clamp(vy, -cap, cap);
            CCRect before = box;
            box.origin.x += dx;
            box.origin.y += vy * gravity;
            for (auto const& o : obstacles) {
                if (!touches(box, o)) continue;
                if (!o.solid) return t;
                // La nave se desliza por encima y por debajo de los bloques
                if (before.getMinY() >= o.rect.getMaxY() - 1.f) {
                    box.origin.y = o.rect.getMaxY() + 0.01f;
                }
                else if (before.getMaxY() <= o.rect.getMinY() + 1.f) {
                    box.origin.y = o.rect.getMinY() - box.size.height - 0.01f;
                }
                else return t;
                vy = 0.f;
            }
        }
        return horizon + 1;
    }

    void clearPending() {
        s_pending[0] = Pending{};
        s_pending[1] = Pending{};
        s_wave[0] = WaveState{};
        s_wave[1] = WaveState{};
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
        return s_cfg.enabled
            && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this;
    }

    PlayerObject* playerFor(int slot) {
        return slot == 1 ? m_player2 : m_player1;
    }

    void markAssist() {
        s_usedAssist = true;
        PlayLayer::get()->m_isTestMode = true;
    }

    void buildIndex() {
        s_index = ObjectIndex{};
        int total = static_cast<int>(m_objects->count());
        for (int i = 0; i < total; i++) {
            auto obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
            auto type = obj->m_objectType;
            if (type != GameObjectType::Solid && type != GameObjectType::Hazard
                && type != GameObjectType::AnimatedHazard) continue;
            if (obj->m_groupCount > 0) s_index.movable.push_back(obj);
            else s_index.fixed.push_back({ obj->getPositionX(), obj });
        }
        std::sort(s_index.fixed.begin(), s_index.fixed.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
        s_index.indexed = total;
    }

    // Bloques y pinchos que estan en el tramo que va a recorrer el jugador
    std::vector<HitObstacle> nearbyObstacles(CCRect const& box, float travel) {
        std::vector<HitObstacle> result;
        if (!m_objects) return result;
        if (s_index.indexed != static_cast<int>(m_objects->count())) this->buildIndex();

        float x = box.getMidX();
        float from = box.getMinX() + std::min(travel, 0.f) - 2.f;
        float to = box.getMaxX() + std::max(travel, 0.f) + 2.f;

        auto add = [&](GameObject* obj) {
            if (obj->m_isNoTouch || obj->m_isGroupDisabled) return;
            float scale = std::max(std::abs(obj->getScaleX()), std::abs(obj->getScaleY()));
            float radius = obj->m_objectRadius * scale;
            CCRect rect = obj->getObjectRect();
            if (std::max(rect.getMaxX(), rect.getMidX() + radius) < from) return;
            if (std::min(rect.getMinX(), rect.getMidX() - radius) > to) return;
            result.push_back({ rect, radius, obj->m_objectType == GameObjectType::Solid });
        };

        auto it = std::lower_bound(s_index.fixed.begin(), s_index.fixed.end(), x - 250.f,
            [](auto const& entry, float value) { return entry.first < value; });
        for (; it != s_index.fixed.end() && it->first <= x + 250.f; ++it) add(it->second);

        for (auto obj : s_index.movable) {
            if (std::abs(obj->getPositionX() - x) <= 250.f) add(obj);
        }
        return result;
    }

    // Si hacer el cambio ahora choca y un poco despues no, devuelve ese retraso
    int predictDelay(PlayerObject* player, WaveState const& wave, bool down, int base) {
        if (player->m_isPlatformer || player->m_isSideways || player->m_isDashing) return base;
        if (wave.step <= 0.01f || wave.step > 20.f) return base;

        int maxDelay = s_cfg.waveMaxDelay;
        int horizon = base + maxDelay + LOOKAHEAD;
        CCRect box = player->getObjectRect();
        float dx = player->m_isGoingLeft ? -wave.step : wave.step;
        auto obstacles = this->nearbyObstacles(box, dx * horizon);
        float dy = player->m_vehicleSize < 1.f ? wave.step * 2.f : wave.step;

        auto hit = [&](int delay) {
            return firstHit(box, obstacles, dx, dy, player->m_isUpsideDown,
                wave.held, down, delay, horizon);
        };

        if (hit(base) > horizon) return base; // ya es seguro
        for (int delay = base + 1; delay <= base + maxDelay; delay++) {
            if (hit(delay) > horizon) return delay;
        }
        return base; // retrasar no lo arregla: no se toca
    }

    void sendWave(WaveState& wave, bool down, bool isPlayer1) {
        wave.held = down;
        wave.heldFor = 0;
        wave.deferred = false;
        GJBaseGameLayer::handleButton(down, 1, isPlayer1);
    }

    int predictShipDelay(PlayerObject* player, WaveState const& wave, bool down, int base) {
        if (player->m_isPlatformer || player->m_isSideways || player->m_isDashing) return base;
        if (wave.step <= 0.01f || wave.step > 20.f || !wave.hasVy) return base;
        auto const& model = s_ship[player->m_vehicleSize < 1.f];
        // Sin datos suficientes todavia no se predice nada
        if (!model.known(wave.held) || !model.known(down)) return base;

        int maxDelay = s_cfg.shipMaxDelay;
        int horizon = base + maxDelay + SHIP_LOOKAHEAD;
        CCRect box = player->getObjectRect();
        float dx = player->m_isGoingLeft ? -wave.step : wave.step;
        auto obstacles = this->nearbyObstacles(box, dx * horizon);

        auto hit = [&](int delay) {
            return firstShipHit(box, obstacles, model, dx, wave.vy,
                player->m_isUpsideDown, wave.held, down, delay, horizon);
        };

        if (hit(base) > horizon) return base;
        for (int delay = base + 1; delay <= base + maxDelay; delay++) {
            if (hit(delay) > horizon) return delay;
        }
        return base;
    }

    // Mide la velocidad y la aceleracion de la nave
    void shipSample(PlayerObject* player, WaveState& wave) {
        if (!player->m_isShip) {
            wave.hasY = wave.hasVy = false;
            wave.taps = 0;
            return;
        }
        bool mini = player->m_vehicleSize < 1.f;
        if (mini != wave.mini) wave.hasY = wave.hasVy = false;
        wave.mini = mini;

        float y = player->getPositionY();
        if (wave.hasY) {
            float gravity = player->m_isUpsideDown ? -1.f : 1.f;
            float vy = (y - wave.lastY) * gravity;
            if (wave.hasVy && wave.prevHeld == wave.held
                && !player->m_isOnGround && !isStraight(player, wave)) {
                float a = vy - wave.vy;
                bool rising = wave.vy > 0.f;
                bool expected = wave.held ? a > 0.001f : a < -0.001f;
                if (expected && std::abs(a) < 1.f) {
                    s_ship[mini].learn(wave.held, rising, a);
                }
                else if (std::abs(a) <= 0.001f && std::abs(vy) > 0.3f) {
                    s_ship[mini].cap[vy > 0.f] = std::abs(vy);
                }
            }
            wave.vy = vy;
            wave.hasVy = true;
            wave.prevHeld = wave.held;
        }
        wave.lastY = y;
        wave.hasY = true;
    }

    void waveButton(int slot, PlayerObject* player, bool down, bool isPlayer1) {
        auto& wave = s_wave[slot];
        // Un evento nuevo: primero se aplica el que estaba esperando
        if (wave.deferred) this->sendWave(wave, wave.deferredDown, wave.isPlayer1);

        bool ship = player->m_isShip;
        if (ship && down && !wave.held) {
            wave.taps = wave.sincePress <= SPAM_GAP ? std::min(wave.taps + 1, 100) : 1;
            wave.sincePress = 0;
        }

        int delay = 0;
        // Volando recto no se retrasa nada, para no cambiar el ritmo
        if (down != wave.held && !isStraight(player, wave)) {
            if (!down) {
                int minTap = ship ? s_cfg.shipMinTap : s_cfg.waveMinTap;
                delay = std::max(0, minTap - wave.heldFor);
            }
            if (ship ? s_cfg.shipPredict : s_cfg.wavePredict) {
                delay = ship
                    ? this->predictShipDelay(player, wave, down, delay)
                    : this->predictDelay(player, wave, down, delay);
            }
        }

        if (delay <= 0) {
            this->sendWave(wave, down, isPlayer1);
            return;
        }
        wave.deferred = true;
        wave.deferredDown = down;
        wave.isPlayer1 = isPlayer1;
        wave.wait = delay;
        this->markAssist();
    }

    // Se llama una vez por tick, antes de la fisica del juego
    void waveTick(int slot, PlayerObject* player) {
        auto& wave = s_wave[slot];
        float x = player->getPositionX();
        if (wave.hasX) wave.step = std::abs(x - wave.lastX);
        wave.lastX = x;
        wave.hasX = true;

        this->shipSample(player, wave);
        if (wave.sincePress < 1000) wave.sincePress++;
        // Vuelo recto: durante el spam se frena la subida y la bajada
        if (!player->m_isDead && isStraight(player, wave)) {
            double strength = s_cfg.straightStrength;
            if (strength > 0) {
                player->m_yVelocity *= 1.0 - strength * 0.0015;
                this->markAssist();
            }
        }

        if (wave.held) wave.heldFor++;
        if (!wave.deferred) return;

        if (player->m_isDead) {
            wave.deferred = false;
        }
        else if (--wave.wait <= 0 || !isFlyer(player)) {
            this->sendWave(wave, wave.deferredDown, wave.isPlayer1);
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // button 1 = salto
        if (button == 1 && this->isActive()) {
            bool twoPlayers = m_levelSettings && m_levelSettings->m_twoPlayerMode;
            int slot = (twoPlayers && !isPlayer1) ? 1 : 0;
            auto player = this->playerFor(slot);
            auto& pending = s_pending[slot];

            if (player && (isFlyer(player) || s_wave[slot].deferred)) {
                pending = Pending{};
                if (isFlyer(player) && !player->m_isDead) {
                    this->waveButton(slot, player, down, isPlayer1);
                    return;
                }
                if (s_wave[slot].deferred) {
                    this->sendWave(s_wave[slot], s_wave[slot].deferredDown, s_wave[slot].isPlayer1);
                }
            }
            s_wave[slot].held = down;
            s_wave[slot].heldFor = 0;

            if (down) {
                pending = Pending{};
                if (player && needsSurface(player) && !player->m_isDead && !canActNow(player)) {
                    pending.ticks = s_cfg.windowTicks;
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
        // Relee los ajustes de vez en cuando por si se cambian en la pausa
        if (--s_reloadIn <= 0) {
            s_cfg.load();
            s_reloadIn = 120;
        }
        if (this->isActive()) {
            for (int slot = 0; slot < 2; slot++) {
                auto& pending = s_pending[slot];
                auto player = this->playerFor(slot);
                if (!player) continue;
                this->waveTick(slot, player);

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
                        this->markAssist();
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
        s_ship[0] = ShipModel{};
        s_ship[1] = ShipModel{};
        s_index = ObjectIndex{};
        s_cfg.load();
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        s_oldTestMode = m_isTestMode;
        return true;
    }

    void resetLevel() {
        clearPending();
        s_cfg.load();
        // Un intento nuevo empieza limpio
        if (s_usedAssist) {
            m_isTestMode = s_oldTestMode;
            s_usedAssist = false;
        }
        PlayLayer::resetLevel();
    }
};
