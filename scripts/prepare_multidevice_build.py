"""Create an isolated, credential-free build configuration; never provision hardware."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
out = root / "build" / "multidevice.sdkconfig"
lines = (root / "sdkconfig").read_text(encoding="utf-8").splitlines()
overrides = {
    "CONFIG_JULIA_MULTI_DEVICE_ENABLE": "y",
    "CONFIG_JULIA_CLOUD_STATE_SYNC_ENABLE": "y",
    "CONFIG_COMM_DEVICE_AUTH_NONE": "n",
    "CONFIG_COMM_DEVICE_AUTH_TOKEN": "n",
    "CONFIG_COMM_DEVICE_AUTH_CERTIFICATE": "n",
    "CONFIG_COMM_DEVICE_AUTH_USERNAME_PASSWORD": "y",
}
result = []
for line in lines:
    match = re.match(r"(?:# )?(CONFIG_\w+)(?:=| is not set)", line)
    if not match:
        result.append(line)
        continue
    key = match[1]
    if key in overrides:
        continue
    # Do not copy private network credentials into the review image or snapshots.
    if any(word in key for word in ("PASSWORD", "TOKEN", "USERNAME", "SSID")) and '="' in line:
        line = key + '=""'
    result.append(line)
for key, value in overrides.items():
    result.append(key + "=" + value if value != "n" else "# " + key + " is not set")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("\n".join(result) + "\n", encoding="utf-8")
print("Created isolated multi-device configuration with empty credentials:", out)
