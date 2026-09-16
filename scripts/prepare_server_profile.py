"""Change only server endpoint settings in a private, provisioned sdkconfig."""
import argparse
from pathlib import Path
import re


def apply_profile(text, legacy):
    values = {
        "JULIA_LEGACY_SERVER_PORTS": "y" if legacy else None,
        "WSS_SERVER_HOST": '"8.133.215.254"',
        "WSS_SERVER_PORT": "9443" if legacy else "19443",
        "WSS_PATH": '"/voice"',
        "COMM_MQTT_BROKER_URI": '"mqtts://8.133.215.254:%s"' % ("1883" if legacy else "11883"),
    }
    for key, value in values.items():
        line = f"CONFIG_{key}={value}" if value else f"# CONFIG_{key} is not set"
        pattern = rf"^(?:CONFIG_{key}=.*|# CONFIG_{key} is not set)$"
        if re.search(pattern, text, re.MULTILINE):
            text = re.sub(pattern, lambda _: line, text, flags=re.MULTILINE)
        else:
            text = text.rstrip("\n") + "\n" + line + "\n"
    return text


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("development", "legacy"), required=True)
    parser.add_argument("--input", type=Path, default=Path("sdkconfig"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    config = apply_profile(args.input.read_text(encoding="utf-8"), args.profile == "legacy")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(config, encoding="utf-8")
    print(f"Prepared {args.profile} endpoint profile; credentials were not printed.")
