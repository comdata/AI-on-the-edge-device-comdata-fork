"""Constants for the AI on the Edge integration."""

DOMAIN = "ai_on_the_edge"

CONF_SCAN_INTERVAL = "scan_interval"
DEFAULT_SCAN_INTERVAL = 60
DEFAULT_PORT = 80

CONF_USE_SSL = "use_ssl"
CONF_VERIFY_SSL = "verify_ssl"

CONF_CAMERA_IMAGE = "camera_image"
DEFAULT_CAMERA_IMAGE = "alg"
# Image files the device serves under /img_tmp/
CAMERA_IMAGES = {"alg": "alg.jpg", "raw": "raw.jpg", "alg_roi": "alg_roi.jpg"}

SERVICE_SET_CONFIG = "set_config"
SERVICE_SET_FALLBACK_VALUE = "set_fallback_value"

ATTR_CONFIG = "config"
ATTR_ENTRY_ID = "entry_id"
ATTR_SEQUENCE = "sequence"
ATTR_VALUE = "value"

# Every other status string is an error, see code/include/defines.h
VALUE_STATUS_VALID = "000 Valid"
