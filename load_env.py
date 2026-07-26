Import("env")

import os


def load_dotenv(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


env_path = os.path.join(env["PROJECT_DIR"], ".env")
dotenv = load_dotenv(env_path)

for key in ("WIFI_SSID", "WIFI_PASSWORD"):
    value = dotenv.get(key)
    if value is None:
        print(f"[load_env] warning: {key} is not set in .env")
        continue
    env.Append(CPPDEFINES=[(key, env.StringifyMacro(value))])
