import time
import shutil
Import("env")

# 1. GERAÇÃO AUTOMÁTICA DA VERSÃO (Timestamp)
timestamp_version = str(int(time.time()))

with open("version.txt", "w") as f:
    f.write(timestamp_version)

print(f"==> Versão automática gerada: {timestamp_version}")

# Injeta a versão no código C++
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", timestamp_version)])

# 2. CÓPIA DO FIRMWARE PARA A RAIZ DO PROJETO
def copy_firmware(source, target, env):
    bin_path = str(target[0])
    shutil.copy(bin_path, "firmware.bin")
    print(f"==> SUCESSO: O arquivo {bin_path} foi copiado para a raiz!")

# Ativa o gatilho pós-compilação
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)