"""Sensor platform for Tigo Monitor devices."""
import logging
import re

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import sensor, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_ADDRESS,
    CONF_DEVICE_ID,
    CONF_NAME,
    CONF_TIME_ID,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_ENERGY,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_WATT,
    UNIT_VOLT,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_KILOWATT_HOURS,
    UNIT_PERCENT
)
from . import tigo_monitor_ns, TigoMonitorComponent, CONF_TIGO_MONITOR_ID

DEPENDENCIES = ['tigo_monitor']

_LOGGER = logging.getLogger(__name__)

# Define specific sensor configs
CONF_POWER_IN = "power_in"
CONF_POWER = "power"  # Legacy alias for power_in
CONF_PEAK_POWER = "peak_power"
CONF_POWER_SUM = "power_sum"
CONF_POWER_OUT_SUM = "power_out_sum"
CONF_ENERGY_SUM = "energy_sum"
CONF_ENERGY_IN_SUM = "energy_in_sum"
CONF_ENERGY_OUT_SUM = "energy_out_sum"
CONF_DEVICE_COUNT = "device_count"
CONF_INVALID_CHECKSUM = "invalid_checksum"
CONF_MISSED_FRAME = "missed_frame"
CONF_VOLTAGE_IN = "voltage_in"
CONF_VOLTAGE_OUT = "voltage_out"
CONF_CURRENT_IN = "current_in"
CONF_CURRENT_OUT = "current_out"
CONF_DUTY_CYCLE = "duty_cycle"
CONF_TEMPERATURE = "temperature"
CONF_RSSI = "rssi"
CONF_BARCODE = "barcode"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_DEVICE_INFO = "device_info"
CONF_EFFICIENCY = "efficiency"
CONF_POWER_FACTOR = "power_factor"
CONF_LOAD_FACTOR = "load_factor"
CONF_POWER_OUT = "power_out"
CONF_STRING_LABEL = "string_label"  # Per-string aggregate sensor (keyed by CCA string label)
# Per-panel energy (lifetime + calendar periods)
CONF_ENERGY = "energy"
CONF_ENERGY_TODAY = "energy_today"
CONF_ENERGY_WEEK = "energy_week"
CONF_ENERGY_MONTH = "energy_month"
CONF_ENERGY_YEAR = "energy_year"
_ENERGY_PERIOD_KEYS = (CONF_ENERGY_TODAY, CONF_ENERGY_WEEK, CONF_ENERGY_MONTH, CONF_ENERGY_YEAR)
# Memory monitoring (ESP32 only)
CONF_INTERNAL_RAM_FREE = "internal_ram_free"
CONF_INTERNAL_RAM_MIN = "internal_ram_min"
CONF_PSRAM_FREE = "psram_free"
CONF_STACK_FREE = "stack_free"

# --- Panel address: short address or barcode (#74) --------------------------
# Mirrors components/tigo_monitor/tigo_sensor_binding.h — keep the two in step.
_SHORT_ADDR_LEN = 4
_BARCODE_MIN_LEN = 6   # a 4-char tail would be indistinguishable from a short address
_BARCODE_MAX_LEN = 16
_PLACEHOLDER_RE = re.compile(r"device_\d+")


def _panel_address(value):
    """Normalise a per-panel `address:`.

    Accepts the 4-hex-char short address the gateway assigned (unchanged
    behaviour), or the panel's barcode: its 16-hex-char long address, or the
    last 6+ chars of it. Dashes, spaces and the web UI's leading "…" are
    dropped and hex is upper-cased, so the node table's "…00899AA0" can be
    pasted as-is. A barcode binding follows the physical panel when the
    gateway renumbers short addresses; a short address does not.
    """
    raw = cv.string(value).strip()
    if _PLACEHOLDER_RE.fullmatch(raw):
        _LOGGER.warning(
            "Panel address '%s' is a YAML-generator placeholder and will never "
            "match a panel — replace it with the panel's barcode.", raw)
        return raw
    key = re.sub(r"[\s\-.\u2026]", "", raw).upper()
    if not re.fullmatch(r"[0-9A-F]+", key or "-"):
        raise cv.Invalid(
            f"Panel address '{raw}' is not hex. Use the panel's barcode as shown "
            "in the web UI node table (e.g. '04C05B4000899AA0' or '…00899AA0'), "
            "or its 4-char short address (e.g. '0004').")
    if len(key) == _SHORT_ADDR_LEN:
        return key
    if _BARCODE_MIN_LEN <= len(key) <= _BARCODE_MAX_LEN:
        return key
    raise cv.Invalid(
        f"Panel address '{raw}' has {len(key)} hex digits. Use the full "
        f"{_BARCODE_MAX_LEN}-digit barcode or at least its last "
        f"{_BARCODE_MIN_LEN} digits (e.g. '899AA0'), or the {_SHORT_ADDR_LEN}-digit "
        "short address. Four digits always mean a short address, which the "
        "gateway can reassign.")


def _is_barcode_key(key):
    return (_BARCODE_MIN_LEN <= len(key) <= _BARCODE_MAX_LEN
            and re.fullmatch(r"[0-9A-F]+", key) is not None)


def _final_validate_energy_clock(config):
    """Calendar-period energy needs the hub's clock.

    Without `time_id` there is no midnight, week or month to roll over at,
    and the sensors would sit at "unknown" forever — the same silent no-op
    #58 was about. Lifetime `energy` needs no clock and stays allowed.
    """
    periods = [k for k in _ENERGY_PERIOD_KEYS if k in config]
    if not periods:
        return config
    hub = fv.full_config.get().get("tigo_monitor") or {}
    if isinstance(hub, list):
        hub = hub[0] if hub else {}
    if CONF_TIME_ID not in hub:
        raise cv.Invalid(
            f"'{periods[0]}' on panel '{config.get(CONF_NAME)}' needs a clock: add "
            "a `time:` block and point `time_id:` under `tigo_monitor:` at it. "
            "(`energy:` — lifetime — works without one.)")
    return config


def _final_validate_barcode_tails(config):
    """Reject barcode keys where one is a tail of another.

    "899AA0" and "00899AA0" name the same panel (or a tail matches two
    panels), and the firmware could only feed one of them. Entries repeating
    the *same* key stay allowed: splitting one panel's sub-sensors across two
    entries has always worked.
    """
    if CONF_ADDRESS not in config:
        return config
    mine = config[CONF_ADDRESS]
    if not _is_barcode_key(mine):
        return config
    for entry in fv.full_config.get().get("sensor", []) or []:
        other = entry.get(CONF_ADDRESS) if isinstance(entry, dict) else None
        if entry.get("platform") != "tigo_monitor" or not other or other == mine:
            continue
        if _is_barcode_key(other) and (other.endswith(mine) or mine.endswith(other)):
            raise cv.Invalid(
                f"Panel addresses '{mine}' and '{other}' overlap: one is the end of "
                "the other, so they can match the same panel. Use one entry per "
                "panel, or give the full barcode.")
    return config


def _tigo_sensor_schema(**kwargs):
    """Create a sensor schema that allows empty configs for auto-templating"""
    base_schema = sensor.sensor_schema(**kwargs)
    # Make id and name optional by removing the validator that requires them
    schema_dict = base_schema.schema.copy()
    # Make ID and NAME optional 
    schema_dict[cv.Optional(CONF_ID)] = cv.declare_id(sensor.Sensor)
    schema_dict[cv.Optional(CONF_NAME)] = cv.string
    return cv.Schema(schema_dict)

def _tigo_text_sensor_schema(**kwargs):
    """Create a text sensor schema that allows empty configs for auto-templating"""
    base_schema = text_sensor.text_sensor_schema(**kwargs)
    # Make id and name optional by removing the validator that requires them
    schema_dict = base_schema.schema.copy()
    # Make ID and NAME optional 
    schema_dict[cv.Optional(CONF_ID)] = cv.declare_id(text_sensor.TextSensor)
    schema_dict[cv.Optional(CONF_NAME)] = cv.string
    return cv.Schema(schema_dict)

def _auto_template_sensor_config(config):
    """Auto-template sensor configs with name and id if not provided"""
    base_name = config[CONF_NAME]
    parent_device_id = config.get(CONF_DEVICE_ID)

    # Sensor configurations with their suffixes
    sensor_configs = [
        (CONF_POWER_IN, "Power In"),
        (CONF_POWER, "Power In"),
        (CONF_PEAK_POWER, "Peak Power"),
        (CONF_POWER_OUT, "Output Power"),
        (CONF_VOLTAGE_IN, "Voltage In"),
        (CONF_VOLTAGE_OUT, "Voltage Out"),
        (CONF_CURRENT_OUT, "Output Current"),
        (CONF_CURRENT_IN, "Current"),
        (CONF_DUTY_CYCLE, "Duty Cycle"),
        (CONF_TEMPERATURE, "Temperature"),
        (CONF_RSSI, "RSSI"),
        (CONF_BARCODE, "Barcode"),
        (CONF_FIRMWARE_VERSION, "Firmware Version"),
        (CONF_DEVICE_INFO, "Device Info"),
        (CONF_EFFICIENCY, "Efficiency"),
        (CONF_POWER_FACTOR, "Power Factor"),
        (CONF_LOAD_FACTOR, "Load Factor"),
        (CONF_ENERGY, "Energy"),
        (CONF_ENERGY_TODAY, "Energy Today"),
        (CONF_ENERGY_WEEK, "Energy Week"),
        (CONF_ENERGY_MONTH, "Energy Month"),
        (CONF_ENERGY_YEAR, "Energy Year"),
    ]

    for conf_key, suffix in sensor_configs:
        if conf_key in config:
            sensor_config = config[conf_key]

            # Auto-generate name if not provided
            if CONF_NAME not in sensor_config:
                sensor_config[CONF_NAME] = f"{base_name} {suffix}"

            # Auto-generate ID if not provided
            if CONF_ID not in sensor_config:
                # Create a valid ID by converting to lowercase and replacing spaces/hyphens with underscores
                base_id = base_name.lower().replace(' ', '_').replace('-', '_')
                suffix_id = suffix.lower().replace(' ', '_').replace('-', '_')

                # Special handling for power sum sensor (no device-specific suffix)
                if conf_key == CONF_POWER_SUM:
                    id_string = f"{base_id}_power_sum"
                else:
                    id_string = f"{base_id}_{suffix_id}"

                # Use appropriate sensor type for ID declaration
                if conf_key in [CONF_BARCODE, CONF_FIRMWARE_VERSION, CONF_DEVICE_INFO]:
                    sensor_config[CONF_ID] = cv.declare_id(text_sensor.TextSensor)(id_string)
                else:
                    sensor_config[CONF_ID] = cv.declare_id(sensor.Sensor)(id_string)

            # Propagate device_id from the parent device entry unless the
            # child sub-sensor explicitly overrides it. Lets users write
            # `device_id: inverter_1` once at the device level instead of
            # repeating it on power_in / peak_power / voltage_in / ...
            if parent_device_id is not None and CONF_DEVICE_ID not in sensor_config:
                sensor_config[CONF_DEVICE_ID] = parent_device_id

            # Add default fields for text sensors (skip None values to avoid C++ generation issues)
            if conf_key in [CONF_BARCODE, CONF_FIRMWARE_VERSION, CONF_DEVICE_INFO]:
                if "disabled_by_default" not in sensor_config:
                    sensor_config["disabled_by_default"] = False

    return config

# Every measurement a per-device entry can ask for. An entry that names none of
# these creates no entities at all, so validation rejects it (see
# _require_device_sub_sensor below).
_DEVICE_SUB_SENSOR_KEYS = (
    CONF_POWER_IN, CONF_POWER, CONF_PEAK_POWER, CONF_POWER_OUT,
    CONF_VOLTAGE_IN, CONF_VOLTAGE_OUT, CONF_CURRENT_IN, CONF_CURRENT_OUT,
    CONF_DUTY_CYCLE, CONF_TEMPERATURE, CONF_RSSI,
    CONF_BARCODE, CONF_FIRMWARE_VERSION, CONF_DEVICE_INFO,
    CONF_EFFICIENCY, CONF_POWER_FACTOR, CONF_LOAD_FACTOR,
    CONF_ENERGY, *_ENERGY_PERIOD_KEYS,
)


def _panel_energy_schema():
    # total_increasing for the period sensors too: HA reads their drop to 0
    # at the start of a period as a meter reset, which is what it is.
    return _tigo_sensor_schema(
        unit_of_measurement=UNIT_KILOWATT_HOURS,
        accuracy_decimals=3,
        device_class=DEVICE_CLASS_ENERGY,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    )


def _require_device_sub_sensor(config):
    """Reject a per-device entry that selects no measurements.

    `address` + `name` only say *which* panel and what to call it — the
    entities come from the sub-keys. Without one, to_code() has nothing to
    create and the entry is a silent no-op, which reads as "my panels never
    showed up in Home Assistant" (issue #48).
    """
    if not any(key in config for key in _DEVICE_SUB_SENSOR_KEYS):
        raise cv.Invalid(
            f"Device sensor '{config[CONF_NAME]}' (address "
            f"'{config[CONF_ADDRESS]}') selects no measurements, so it would "
            "create no entities. Add at least one sub-key, e.g.:\n"
            "    power: {}\n"
            "    voltage_in: {}\n"
            "    current_in: {}\n"
            "    temperature: {}\n"
            "Available: " + ", ".join(_DEVICE_SUB_SENSOR_KEYS)
        )
    return config


# Schema for individual device sensors
DEVICE_CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
        cv.Required(CONF_ADDRESS): _panel_address,
        cv.Required(CONF_NAME): cv.string,
        # Optional sub-device assignment. Set once here and it propagates to
        # every selected sub-sensor below (power_in, peak_power, ...). The
        # per-sensor `device_id` still wins if you want to override one.
        cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
        cv.Optional(CONF_POWER_IN): _tigo_sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER): _tigo_sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PEAK_POWER): _tigo_sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_IN): _tigo_sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_OUT): _tigo_sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT_IN): _tigo_sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT_OUT): _tigo_sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_DUTY_CYCLE): _tigo_sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMPERATURE): _tigo_sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_RSSI): _tigo_sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BARCODE): _tigo_text_sensor_schema(),
        cv.Optional(CONF_FIRMWARE_VERSION): _tigo_text_sensor_schema(),
        cv.Optional(CONF_DEVICE_INFO): _tigo_text_sensor_schema(),
        cv.Optional(CONF_EFFICIENCY): _tigo_sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER_OUT): _tigo_sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER_FACTOR): _tigo_sensor_schema(
            accuracy_decimals=3,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_LOAD_FACTOR): _tigo_sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_ENERGY): _panel_energy_schema(),
        cv.Optional(CONF_ENERGY_TODAY): _panel_energy_schema(),
        cv.Optional(CONF_ENERGY_WEEK): _panel_energy_schema(),
        cv.Optional(CONF_ENERGY_MONTH): _panel_energy_schema(),
        cv.Optional(CONF_ENERGY_YEAR): _panel_energy_schema(),
    }).extend(cv.COMPONENT_SCHEMA),
    _require_device_sub_sensor,
    _auto_template_sensor_config,
)

# Schema for input power sum sensor (no address required)
POWER_IN_SUM_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for a per-string power sensor — keyed by the CCA string label instead
# of a device address. One entity per string instead of one per panel; on large
# installs each dropped panel entity saves ~110-150 B of internal RAM.
STRING_POWER_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
    cv.Required(CONF_STRING_LABEL): cv.string_strict,
}).extend(cv.COMPONENT_SCHEMA)

# Schema for power output sum sensor (no address required)
POWER_OUT_SUM_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for input energy sum sensor (no address required)
ENERGY_IN_SUM_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for output energy sum sensor (no address required)
ENERGY_OUT_SUM_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for device count sensor (no address required)
DEVICE_COUNT_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for invalid checksum counter (no address required)
INVALID_CHECKSUM_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:alert-circle-outline",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Schema for missed frame counter (no address required)
MISSED_FRAME_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:alert-circle-outline",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Memory monitoring sensors (ESP32 only)
INTERNAL_RAM_FREE_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="KB",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

INTERNAL_RAM_MIN_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="KB",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# Alert-count sensors: stale panels and fresh-but-zero-production panels (#24)
STALE_COUNT_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:lan-disconnect",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

ZERO_PRODUCTION_COUNT_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:solar-panel",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

PSRAM_FREE_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="KB",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

STACK_FREE_CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="B",
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:memory",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
}).extend(cv.COMPONENT_SCHEMA)

# --- Aggregate (no-address) sensor classification --------------------------
# Aggregate sensors carry no `address`, so their type is inferred from keywords
# in the `name`. Matching is WHOLE-WORD (regex \b) so a substring can't cross-
# trigger another category — e.g. "sum" must not match inside "checksum", and
# "e in" must not match inside "free internal". Rule order matters: the first
# match wins, so more specific categories (energy/power out, checksum, frame)
# are listed before broader ones (generic power, count). In particular,
# checksum/frame precede count so names like "Invalid Checksum Count" /
# "Missed Frame Count" are not swallowed by the generic "count" keyword.
_AGGREGATE_RULES = [
    # Alert counts first — their names ("Stale Panel Count", "Zero Production
    # Count") contain words the broader rules below would otherwise swallow.
    ("stale_count",     ["stale"]),
    ("zero_production", ["zero production", "no production", "zero output",
                         "dead", "failed"]),
    ("energy_out",   ["energy out", "output energy", "e_out", "e out"]),
    ("energy_in",    ["energy in", "input energy", "e_in", "e in",
                      "energy", "kwh", "kilowatt", "wh"]),
    ("power_out",    ["output power", "power out", "p_out", "p out"]),
    ("power_in",     ["input power", "power in", "p_in", "p in", "power",
                      "watt", "total", "sum", "combined", "system"]),
    ("checksum",     ["checksum", "invalid", "crc", "error"]),
    ("frame",        ["frame", "missed", "lost", "dropped"]),
    ("count",        ["count", "devices", "discovered", "active", "number"]),
    ("psram",        ["psram"]),
    ("stack",        ["stack"]),
    ("internal_ram", ["internal", "ram", "heap"]),
]

_MIN_KEYWORDS = ["min", "minimum", "watermark"]


def _name_has(name, keywords):
    """True if any keyword matches `name` as a whole word/phrase.

    Whole-word matching avoids substring false positives such as "sum"
    matching inside "checksum" or "e in" matching inside "free internal".
    """
    return any(re.search(r"\b" + re.escape(kw) + r"\b", name) for kw in keywords)


def _classify_aggregate_sensor(name):
    """Return the aggregate-sensor category for a no-address sensor name.

    Returns None when no category matches.
    """
    name = name.lower()
    for category, keywords in _AGGREGATE_RULES:
        if _name_has(name, keywords):
            if category == "internal_ram" and _name_has(name, _MIN_KEYWORDS):
                return "internal_ram_min"
            return category
    return None


_AGGREGATE_SCHEMA_BY_CATEGORY = {
    "stale_count": STALE_COUNT_CONFIG_SCHEMA,
    "zero_production": ZERO_PRODUCTION_COUNT_CONFIG_SCHEMA,
    "energy_out": ENERGY_OUT_SUM_CONFIG_SCHEMA,
    "energy_in": ENERGY_IN_SUM_CONFIG_SCHEMA,
    "power_out": POWER_OUT_SUM_CONFIG_SCHEMA,
    "power_in": POWER_IN_SUM_CONFIG_SCHEMA,
    "checksum": INVALID_CHECKSUM_CONFIG_SCHEMA,
    "frame": MISSED_FRAME_CONFIG_SCHEMA,
    "count": DEVICE_COUNT_CONFIG_SCHEMA,
    "psram": PSRAM_FREE_CONFIG_SCHEMA,
    "stack": STACK_FREE_CONFIG_SCHEMA,
    "internal_ram": INTERNAL_RAM_FREE_CONFIG_SCHEMA,
    "internal_ram_min": INTERNAL_RAM_MIN_CONFIG_SCHEMA,
}


# Main config schema that handles both types
def _validate_config(config):
    """Validate configuration and determine sensor type."""
    if CONF_STRING_LABEL in config:
        # Per-string aggregate sensor — keyed by CCA string label.
        if CONF_ADDRESS in config:
            raise cv.Invalid(
                "'string_label' and 'address' are mutually exclusive: use "
                "'address' for a per-panel sensor or 'string_label' for a "
                "per-string aggregate."
            )
        return STRING_POWER_CONFIG_SCHEMA(config)

    if CONF_ADDRESS in config:
        # Device sensor — keyed by address.
        return DEVICE_CONFIG_SCHEMA(config)

    # Aggregate sensor — type is inferred from name keywords.
    category = _classify_aggregate_sensor(config.get(CONF_NAME, ""))
    schema = _AGGREGATE_SCHEMA_BY_CATEGORY.get(category)
    if schema is None:
        raise cv.Invalid(
            "For sensors without 'address', the name must contain a keyword "
            "identifying the aggregate type: 'energy out'/'output energy' for "
            "output-energy, 'energy'/'kwh'/'energy in' for input-energy, "
            "'power out'/'output power' for output-power, "
            "'power'/'total'/'sum'/'power in' for input-power, "
            "'count'/'devices' for device count, 'checksum'/'invalid' for "
            "checksum errors, 'frame'/'missed' for frame errors, 'psram' for "
            "PSRAM free, 'stack' for stack free, 'internal ram min' for "
            "internal RAM minimum, or 'internal ram' for internal RAM free."
        )
    return schema(config)

CONFIG_SCHEMA = _validate_config

def _final_validate(config):
    _final_validate_barcode_tails(config)
    _final_validate_energy_clock(config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

async def to_code(config):
    hub = await cg.get_variable(config[CONF_TIGO_MONITOR_ID])

    # Per-string aggregate sensor — published from update_string_data()
    if CONF_STRING_LABEL in config:
        sens = await sensor.new_sensor(config)
        cg.add(hub.add_string_power_sensor(config[CONF_STRING_LABEL], sens))
        return

    # Check if this is an aggregate sensor (no address) or device sensor (has address)
    if CONF_ADDRESS not in config:
        # Aggregate sensor — route by the same name-keyword classifier used in
        # validation, so the registered hub method always matches the schema.
        category = _classify_aggregate_sensor(config.get(CONF_NAME, ""))
        sens = await sensor.new_sensor(config)
        register = {
            "stale_count": hub.add_stale_count_sensor,
            "zero_production": hub.add_zero_production_count_sensor,
            "energy_out": hub.add_energy_out_sum_sensor,
            "energy_in": hub.add_energy_in_sum_sensor,
            "power_out": hub.add_power_out_sum_sensor,
            "power_in": hub.add_power_sum_sensor,
            "checksum": hub.add_invalid_checksum_sensor,
            "frame": hub.add_missed_frame_sensor,
            "count": hub.add_device_count_sensor,
            "psram": hub.add_psram_free_sensor,
            "stack": hub.add_stack_free_sensor,
            "internal_ram": hub.add_internal_ram_free_sensor,
            "internal_ram_min": hub.add_internal_ram_min_sensor,
        }.get(category, hub.add_power_sum_sensor)
        cg.add(register(sens))
        return
    
    # This is a device sensor configuration
    address = config[CONF_ADDRESS]
    cg.add(hub.add_panel_binding(address))
    
    # Define sensor configurations with their methods
    sensor_configs = [
        (CONF_POWER_IN, hub.add_power_in_sensor, sensor.new_sensor),
        (CONF_POWER, hub.add_power_in_sensor, sensor.new_sensor),
        (CONF_PEAK_POWER, hub.add_peak_power_sensor, sensor.new_sensor),
        (CONF_POWER_OUT, hub.add_power_out_sensor, sensor.new_sensor),
        (CONF_VOLTAGE_IN, hub.add_voltage_in_sensor, sensor.new_sensor),
        (CONF_VOLTAGE_OUT, hub.add_voltage_out_sensor, sensor.new_sensor),
        (CONF_CURRENT_OUT, hub.add_current_out_sensor, sensor.new_sensor),
        (CONF_CURRENT_IN, hub.add_current_in_sensor, sensor.new_sensor),
        (CONF_DUTY_CYCLE, hub.add_duty_cycle_sensor, sensor.new_sensor),
        (CONF_TEMPERATURE, hub.add_temperature_sensor, sensor.new_sensor),
        (CONF_RSSI, hub.add_rssi_sensor, sensor.new_sensor),
        (CONF_BARCODE, hub.add_barcode_sensor, text_sensor.new_text_sensor),
        (CONF_FIRMWARE_VERSION, hub.add_firmware_version_sensor, text_sensor.new_text_sensor),
        (CONF_DEVICE_INFO, hub.add_device_info_sensor, text_sensor.new_text_sensor),
        (CONF_EFFICIENCY, hub.add_efficiency_sensor, sensor.new_sensor),
        (CONF_POWER_FACTOR, hub.add_power_factor_sensor, sensor.new_sensor),
        (CONF_LOAD_FACTOR, hub.add_load_factor_sensor, sensor.new_sensor),
        (CONF_ENERGY, hub.add_panel_energy_sensor, sensor.new_sensor),
        (CONF_ENERGY_TODAY, hub.add_panel_energy_today_sensor, sensor.new_sensor),
        (CONF_ENERGY_WEEK, hub.add_panel_energy_week_sensor, sensor.new_sensor),
        (CONF_ENERGY_MONTH, hub.add_panel_energy_month_sensor, sensor.new_sensor),
        (CONF_ENERGY_YEAR, hub.add_panel_energy_year_sensor, sensor.new_sensor),
    ]
    
    # Process each configured sensor type (auto-templating already done in validation)
    for conf_key, add_method, new_sensor_method in sensor_configs:
        if conf_key in config:
            sensor_config = config[conf_key]
            # Create the sensor object from the config
            sens = await new_sensor_method(sensor_config)
            # Register sensor with the hub using cg.add() to generate C++ code
            cg.add(add_method(address, sens))