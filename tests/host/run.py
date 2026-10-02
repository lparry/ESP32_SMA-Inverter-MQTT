#!/usr/bin/env python3
"""Compile and run production code with deterministic hardware fakes; no ESP32 required."""
import pathlib, subprocess, tempfile, shutil, os
ROOT = pathlib.Path(__file__).resolve().parents[2]
JSON = ROOT / '.pio/libdeps/espwroom32/ArduinoJson/src'
if not JSON.exists():
    raise SystemExit('Install project libraries first: pio pkg install -e espwroom32')
with tempfile.TemporaryDirectory(prefix='sma-host-') as tmp:
    tmp = pathlib.Path(tmp)
    sources = tmp/'src'
    sources.mkdir()
    for f in (ROOT/'src').iterdir():
        if f.suffix in ('.cpp','.h') and f.name not in ('config_values.h','Config.h'):
            shutil.copy(f,sources/f.name)
    # Exercise missing, example, and partial private configurations independently.
    defaults = tmp/'defaults.cpp'
    defaults.write_text('#include "ConfigDefaults.h"\nstatic_assert(MQTT_PORT==1883 && SUNUP==6 && SUNDOWN==18 && NIGHTSCANRATE==900000, "defaults");\nint main(){}\n')
    for override in (None, (ROOT/'src/Config_example.h').read_text(), '#define MQTT_BROKER "custom"\n'):
        config = sources/'config_values.h'
        if override is None:
            config.unlink(missing_ok=True)
        else:
            config.write_text(override)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-I'+str(sources),str(defaults),'-o',str(tmp/'defaults')],check=True)
    shutil.copy(ROOT/'tests/host/config.h',sources/'config_values.h')
    cmd = [os.environ.get('CXX','c++'),'-std=c++17','-pthread','-Wno-register','-Werror=format','-Wno-deprecated-declarations','-Wno-parentheses-equality','-Wno-comment','-g','-fno-access-control',
           '-fsanitize=address,undefined','-fno-omit-frame-pointer',
           '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1','-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
           '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0',
           '-I'+str(ROOT/'tests/host/stubs'),'-I'+str(sources),'-I'+str(JSON),
           *[str(sources/n) for n in ('SMA_Utils.cpp','SMA_Inverter.cpp','ESP32_SMA_Inverter_App.cpp','ESP32_SMA_MQTT.cpp')],str(ROOT/'tests/host/test.cpp'),'-o',str(tmp/'tests')]
    subprocess.run(cmd,check=True)
    subprocess.run([str(tmp/'tests')],check=True)
