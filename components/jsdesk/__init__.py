import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, sensor, text_sensor, uart
from esphome.const import (
    CONF_HEIGHT,
    CONF_ID,
    DEVICE_CLASS_DISTANCE,
    ICON_ARROW_EXPAND_VERTICAL,
    STATE_CLASS_MEASUREMENT,
    UNIT_CENTIMETER,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]

jsdesk_ns = cg.esphome_ns.namespace("jsdesk")
JSDesk = jsdesk_ns.class_("JSDesk", cg.Component)

CONF_PANEL_UART = "panel_uart"
CONF_BOX_UART = "box_uart"
CONF_BOX_TX_UART = "box_tx_uart"
CONF_KEY_IN_PIN = "key_in_pin"
CONF_KEY_OUT_PIN = "key_out_pin"
CONF_KEY2_IN_PIN = "key2_in_pin"
CONF_MIN_HEIGHT = "min_height"
CONF_MAX_HEIGHT = "max_height"
CONF_STOP_TOLERANCE = "stop_tolerance"
CONF_KEY_PULSE = "key_pulse"
CONF_KEY_ON_MOVE = "key_on_move"
CONF_LOG_FRAMES = "log_frames"
CONF_DISPLAY = "display"
CONF_KEY1 = "key1"
CONF_KEY2 = "key2"

# Button bits in the panel -> box frame (A5 00 <buttons> <~buttons> FF)
BUTTONS = {
    "up": 0x20,
    "down": 0x40,
    "memory1": 0x02,
    "memory2": 0x04,
    "memory3": 0x08,
    "memory4": 0x10,
}


def _validate(config):
    if config[CONF_MIN_HEIGHT] >= config[CONF_MAX_HEIGHT]:
        raise cv.Invalid("min_height must be lower than max_height")
    if CONF_PANEL_UART not in config and CONF_BOX_TX_UART not in config:
        raise cv.Invalid("Without panel_uart, box_tx_uart is required to poll the box")
    if CONF_KEY_OUT_PIN in config and CONF_BOX_TX_UART not in config:
        raise cv.Invalid("key_out_pin is only used together with box_tx_uart (passthrough wiring)")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(JSDesk),
            # RX: panel -> box line (T). Omit when no panel is connected (standalone).
            cv.Optional(CONF_PANEL_UART): cv.use_id(uart.UARTComponent),
            # RX: box -> panel line (R), tapped in parallel. Required.
            cv.Required(CONF_BOX_UART): cv.use_id(uart.UARTComponent),
            # TX: ESP -> box (T line cut, ESP in the middle). Enables control.
            cv.Optional(CONF_BOX_TX_UART): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_KEY_IN_PIN): pins.gpio_input_pin_schema,
            cv.Optional(CONF_KEY_OUT_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_KEY2_IN_PIN): pins.gpio_input_pin_schema,
            cv.Optional(CONF_MIN_HEIGHT, default=60.0): cv.float_,
            cv.Optional(CONF_MAX_HEIGHT, default=125.0): cv.float_,
            cv.Optional(CONF_STOP_TOLERANCE, default=0.5): cv.float_range(min=0.0, max=5.0),
            # Drive KEY1 high while the ESP moves the desk. Not needed on the Q200-23,
            # where KEY1 alone moves the desk; there it is only mirrored from the panel.
            cv.Optional(CONF_KEY_ON_MOVE, default=False): cv.boolean,
            # With key_on_move: unset holds KEY1 for the whole move, set pulses it.
            cv.Optional(CONF_KEY_PULSE): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LOG_FRAMES, default=False): cv.boolean,
            cv.Optional(CONF_HEIGHT): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                icon=ICON_ARROW_EXPAND_VERTICAL,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_DISTANCE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_DISPLAY): text_sensor.text_sensor_schema(icon="mdi:numeric"),
            cv.Optional(CONF_KEY1): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_KEY2): binary_sensor.binary_sensor_schema(),
            **{cv.Optional(key): binary_sensor.binary_sensor_schema() for key in BUTTONS},
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_PANEL_UART in config:
        cg.add(var.set_panel_uart(await cg.get_variable(config[CONF_PANEL_UART])))
    cg.add(var.set_box_uart(await cg.get_variable(config[CONF_BOX_UART])))
    if CONF_BOX_TX_UART in config:
        cg.add(var.set_box_tx_uart(await cg.get_variable(config[CONF_BOX_TX_UART])))

    if CONF_KEY_IN_PIN in config:
        cg.add(var.set_key_in_pin(await cg.gpio_pin_expression(config[CONF_KEY_IN_PIN])))
    if CONF_KEY_OUT_PIN in config:
        cg.add(var.set_key_out_pin(await cg.gpio_pin_expression(config[CONF_KEY_OUT_PIN])))
    if CONF_KEY2_IN_PIN in config:
        cg.add(var.set_key2_in_pin(await cg.gpio_pin_expression(config[CONF_KEY2_IN_PIN])))

    cg.add(var.set_min_height(config[CONF_MIN_HEIGHT]))
    cg.add(var.set_max_height(config[CONF_MAX_HEIGHT]))
    cg.add(var.set_stop_tolerance(config[CONF_STOP_TOLERANCE]))
    cg.add(var.set_key_on_move(config[CONF_KEY_ON_MOVE]))
    if CONF_KEY_PULSE in config:
        cg.add(var.set_key_pulse(config[CONF_KEY_PULSE]))
    cg.add(var.set_log_frames(config[CONF_LOG_FRAMES]))

    if CONF_HEIGHT in config:
        cg.add(var.set_height_sensor(await sensor.new_sensor(config[CONF_HEIGHT])))
    if CONF_DISPLAY in config:
        cg.add(var.set_display_sensor(await text_sensor.new_text_sensor(config[CONF_DISPLAY])))
    if CONF_KEY1 in config:
        cg.add(var.set_key1_sensor(await binary_sensor.new_binary_sensor(config[CONF_KEY1])))
    if CONF_KEY2 in config:
        cg.add(var.set_key2_sensor(await binary_sensor.new_binary_sensor(config[CONF_KEY2])))
    for key, mask in BUTTONS.items():
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(var.add_button_sensor(mask, bs))
