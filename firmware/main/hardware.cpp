// hardware.cpp — Accès matériel (variante différentielle : 2 moteurs avant + 2 AS5600).
// Les handles des drivers (ADC, LEDC, 2× I2C) vivent ici en statique ; le reste du firmware
// passe par les fonctions libres du namespace `board`.
#include "hardware.hpp"

#include <atomic>
#include "soc/dport_reg.h"

#include <cmath>

#include "ads1115.hpp"
#include "config.hpp"
#include "pinout.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char* TAG = "board";

// Sens de rotation (niveau appliqué sur la broche DIR du driver).
enum class Dir : int { Forward = 1, Reverse = 0 };

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

namespace
{
// Résolution du timer LEDC (type esp_driver_ledc) — la valeur numérique correspondante
// (PWM_MAX = 4095) vit dans control_types.hpp, seule utile à la logique.
constexpr ledc_timer_bit_t PWM_RES = LEDC_TIMER_12_BIT;

i2c_master_bus_handle_t   m_bus[2] = {nullptr, nullptr};   // bus 0 = roue G, bus 1 = roue D
i2c_master_dev_handle_t   m_as[2]  = {nullptr, nullptr};   // AS5600 par bus
int                       m_angle_last[2] = {-1, -1};      // dernier angle brut par capteur
Ads1115                   m_ads;                           // ADC externe (bus 0), Vbat sur A0
bool                      m_led_on = false;

// SENTINELLE horloge LEDC — diagnostic + auto-réparation de la course soupçonnée :
// crash récurrent « Interrupt WDT » avec PC figé sur une ÉCRITURE de registre LEDC, sous
// charge radio (Wi-Fi/BT). Hypothèse : un read-modify-write concurrent (non verrouillé côté
// blobs radio) sur DPORT_PERIP_CLK_EN_REG perd le bit d'horloge du LEDC ; or écrire dans un
// périphérique SANS horloge fige le bus APB sur ESP32 → watchdog d'interruption sans dump.
// On vérifie le bit AVANT chaque écriture moteur : coupé → réparé + compté (page Système).
std::atomic<uint32_t> m_ledc_clk_fix{0};

inline void ledcClockGuard()
{
    if (0 == (DPORT_REG_READ(DPORT_PERIP_CLK_EN_REG) & DPORT_LEDC_CLK_EN))
    {
        DPORT_REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_LEDC_CLK_EN);
        DPORT_REG_CLR_BIT(DPORT_PERIP_RST_EN_REG, DPORT_LEDC_RST);
        const uint32_t n = m_ledc_clk_fix.fetch_add(1) + 1;
        ESP_LOGE(TAG, "Horloge LEDC trouvée COUPÉE (course DPORT/radio ?) — réparée (n=%lu)",
                 static_cast<unsigned long>(n));
    }
}

// Caches d'état des sorties : on n'écrit LEDC/GPIO QUE si la valeur change. À 500 Hz en
// freinage permanent, ça élimine ~2000 écritures APB/s inutiles (et réduit d'autant la
// fenêtre d'exposition à la course d'horloge ci-dessus).
uint32_t m_duty_last[2] = {UINT32_MAX, UINT32_MAX};
int      m_dir_last[2]  = {-1, -1};

void dirPin(gpio_num_t pin, Dir d)
{
    const int idx = (pins::DIR_L == pin) ? 0 : 1;
    if (m_dir_last[idx] == static_cast<int>(d)) return;
    m_dir_last[idx] = static_cast<int>(d);
    gpio_set_level(pin, static_cast<int>(d));
}

void setDuty(ledc_channel_t ch, uint32_t duty)
{
    const int idx = (LEDC_CHANNEL_0 == ch) ? 0 : 1;
    if (m_duty_last[idx] == duty) return;
    ledcClockGuard();
    m_duty_last[idx] = duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

void initLED()
{
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::LED);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
}

// ADC externe ADS1115 (sur le bus 0). Vbat suivie en continu sur A0 en ±4,096 V
// (résolution 125 µV) : la tension à la broche (≤ 3,3 V via le diviseur) tient largement.
void initExtAdc()
{
    if (!m_ads.begin(m_bus[0], hw::ADS1115_ADDR, hw::I2C_FREQ_HZ))
    {
        ESP_LOGW(TAG, "ADS1115 indisponible : mesure Vbat à 0");
        return;
    }
    m_ads.startContinuous(pins::ads::VBAT, Ads1115::Gain::FS_4V096, Ads1115::Rate::SPS_128);
}

void initMotors()
{
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = PWM_RES;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = hw::PWM_FREQ_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    auto configChannel = [](ledc_channel_t ch, gpio_num_t pin)
    {
        ledc_channel_config_t c{};
        c.gpio_num = pin;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel = ch;
        c.timer_sel = LEDC_TIMER_0;
        c.duty = 0;
        c.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    };
    configChannel(LEDC_CHANNEL_0, pins::PWM_L);
    configChannel(LEDC_CHANNEL_1, pins::PWM_R);

    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::DIR_L) | (1ULL << pins::DIR_R);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    // L'attache LEDC reconfigure les broches PWM : réarmer leurs pull-down internes.
    gpio_pulldown_en(pins::PWM_L);
    gpio_pulldown_en(pins::PWM_R);
    dirPin(pins::DIR_L, Dir::Forward);
    dirPin(pins::DIR_R, Dir::Forward);
    board::motorsBrake();   // état par défaut : freinage dynamique (jamais en roue libre)
}

// Deux bus I2C indépendants, un capteur AS5600 (0x36) par bus.
void initEncoders()
{
    struct { i2c_port_t port; gpio_num_t sda; gpio_num_t scl; } cfg[2] = {
        {I2C_NUM_0, pins::I2C0_SDA, pins::I2C0_SCL},
        {I2C_NUM_1, pins::I2C1_SDA, pins::I2C1_SCL},
    };
    for (int i = 0; i < 2; ++i)
    {
        i2c_master_bus_config_t bus{};
        bus.i2c_port = cfg[i].port;
        bus.sda_io_num = cfg[i].sda;
        bus.scl_io_num = cfg[i].scl;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        if (ESP_OK != i2c_new_master_bus(&bus, &m_bus[i]))
        {
            ESP_LOGW(TAG, "Bus I2C %d indisponible : pas de capteur roue %c", i, i ? 'D' : 'G');
            continue;
        }
        i2c_device_config_t dev{};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = hw::AS5600_ADDR;
        dev.scl_speed_hz = hw::I2C_FREQ_HZ;
        if (ESP_OK != i2c_master_bus_add_device(m_bus[i], &dev, &m_as[i]))
        {
            ESP_LOGW(TAG, "AS5600 roue %c non ajouté", i ? 'D' : 'G');
            m_as[i] = nullptr;
        }
    }
}

// Présence de chaque AS5600 : dernière lecture I2C réussie ? (rafraîchi à 500 Hz par la
// boucle de contrôle via angleDelta → alimente les défauts « encodeur absent » par roue).
std::atomic<bool> m_enc_present[2] = {false, false};

// Angle brut 12 bits (0..4095) du capteur `i`. Retourne -1 si absent/erreur.
int readAngleRaw(int i)
{
    if (!m_as[i]) return -1;
    const uint8_t reg = hw::AS5600_REG_RAWANG;
    uint8_t buf[2] = {0, 0};
    if (ESP_OK != i2c_master_transmit_receive(m_as[i], &reg, 1, buf, 2, 20))
    {
        m_enc_present[i].store(false);
        return -1;
    }
    m_enc_present[i].store(true);
    return ((buf[0] & 0x0F) << 8) | buf[1];
}

// Δangle signé du capteur `i` depuis le dernier appel, wrap 0↔4095 → [-2048..2047].
int angleDelta(int i)
{
    const int cur = readAngleRaw(i);
    if (cur < 0) return 0;
    if (m_angle_last[i] < 0)
    {
        m_angle_last[i] = cur;
        return 0;
    }
    int d = cur - m_angle_last[i];
    m_angle_last[i] = cur;
    if (d > 2048)  d -= 4096;
    if (d < -2048) d += 4096;
    return d;
}

void initButton()
{
    gpio_config_t in{};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << pins::START_BTN);
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);
}

// Anti-rebond : un état ne change qu'après BTN_DEBOUNCE_TICKS lectures stables.
struct Debounce
{
    bool state = false;
    int  count = 0;
};
Debounce m_db_start;

bool debounce(Debounce& d, bool raw)
{
    if (raw == d.state)
    {
        d.count = 0;
    }
    else if (++d.count >= hw::BTN_DEBOUNCE_TICKS)
    {
        d.state = raw;
        d.count = 0;
    }
    return d.state;
}


void motorApply(ledc_channel_t ch, gpio_num_t dir, float v, uint32_t cap)
{
    dirPin(dir, (v >= 0) ? Dir::Forward : Dir::Reverse);
    setDuty(ch, static_cast<uint32_t>(fabsf(clampf(v, -1.f, 1.f)) * cap));
}
} // namespace

// ───────────────────────────── API publique ─────────────────────────────
void board::init()
{
    initLED();
    initMotors();
    initEncoders();   // 2 bus I2C + 2 capteurs AS5600
    initExtAdc();     // ADS1115 sur le bus 0 (après création des bus)
    initButton();
    board::led(false);
}

void board::led(bool on)
{
    gpio_set_level(pins::LED, on ? 1 : 0);
    m_led_on = on;
}

void board::ledToggle()
{
    board::led(!m_led_on);
}

float board::vbatVolts(int n)
{
    if (!m_ads.ok())
    {
        return -1.0f;   // capteur absent → tension inconnue (le contrôleur saute la LVC)
    }
    // Moyenne de n lectures du registre de conversion (mode continu) → tension à la broche A0.
    long acc = 0;
    int  got = 0;
    for (int k = 0; k < n; ++k)
    {
        int16_t raw = 0;
        if (m_ads.readRaw(raw))
        {
            acc += raw;
            ++got;
        }
    }
    if (0 == got)
    {
        return 0.0f;
    }
    return m_ads.toVolts(static_cast<int16_t>(acc / got));
}

void board::motorsSet(float l, float r, uint32_t cap)
{
    motorApply(LEDC_CHANNEL_0, pins::DIR_L, l, cap);
    motorApply(LEDC_CHANNEL_1, pins::DIR_R, r, cap);
}

void board::motorsStop()
{
    setDuty(LEDC_CHANNEL_0, 0);
    setDuty(LEDC_CHANNEL_1, 0);
}

void board::motorsBrake()
{
    // Freinage dynamique passif : rapport cyclique nul + DIR bas sur les 2 canaux → les deux
    // sorties de chaque pont sont basses → moteur court-circuité (résiste au mouvement).
    setDuty(LEDC_CHANNEL_0, 0);
    setDuty(LEDC_CHANNEL_1, 0);
    dirPin(pins::DIR_L, Dir::Reverse);
    dirPin(pins::DIR_R, Dir::Reverse);
}

uint32_t board::ledcClkFixCount() { return m_ledc_clk_fix.load(); }

int board::encLeftDelta()  { return angleDelta(0); }
int board::encRightDelta() { return angleDelta(1); }
bool board::encLeftPresent()  { return m_enc_present[0].load(); }
bool board::encRightPresent() { return m_enc_present[1].load(); }

void board::pollButtons()
{
    debounce(m_db_start, pins::BTN_ACTIVE == gpio_get_level(pins::START_BTN));
}

bool board::btnStart()
{
    return m_db_start.state;
}

void board::powerLatch()
{
    // Actif BAS : tirer la LED de l'opto vers la masse → l'opto envoie le +20 V sur la gate.
    gpio_set_level(pins::POWER_HOLD, 0);
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::POWER_HOLD);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;   // actif BAS : même en haute impédance, rester « maintenu »
    gpio_config(&io);
    gpio_set_level(pins::POWER_HOLD, 0);
}

void board::powerOff()
{
    gpio_set_level(pins::POWER_HOLD, 1);
}

void board::motorsIdleEarly()
{
    // PWM/DIR en sortie à l'état BAS dès le boot, avant l'init LEDC : tout bas = FREINAGE
    // DYNAMIQUE (les deux sorties du pont au niveau bas court-circuitent le moteur). Les
    // PULL-DOWN internes sont armés en plus : si une broche repasse en haute impédance
    // pendant que la puce tourne, elle retombe côté frein. ⚠️ Un RESET efface ces pulls
    // (registres IO_MUX) : le défaut électrique pendant le bootloader dépend des pull-down
    // EXTERNES du driver — à garantir côté câblage (voir README).
    gpio_config_t io{};
    io.pin_bit_mask = (1ULL << pins::PWM_L) | (1ULL << pins::DIR_L) |
                      (1ULL << pins::PWM_R) | (1ULL << pins::DIR_R);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    gpio_set_level(pins::PWM_L, 0);
    gpio_set_level(pins::DIR_L, 0);
    gpio_set_level(pins::PWM_R, 0);
    gpio_set_level(pins::DIR_R, 0);
}
