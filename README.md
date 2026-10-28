# MineSwitch

**Minecraft: Java Edition 1.21.1 natively on Nintendo Switch** (Horizon OS, Atmosphère).
No Linux, no emulation, no streaming: a real OpenJDK 21 HotSpot with C1/C2 JIT
ported to Horizon, Vulkan through a Mesa/NVK fork (NXVK) with Zink for OpenGL, and
a PolyMC-style launcher that runs on the console.

> Порт Minecraft Java 1.21.1 на Nintendo Switch, работающий нативно в Horizon OS.
> Документация проекта в основном на русском.

## Состояние

Проверено на Switch OLED (Atmosphère, режим приложения):

| Область | Результат |
| --- | --- |
| JVM | OpenJDK 21 HotSpot для `aarch64-unknown-horizon`: шаблонный интерпретатор, C1 и C2, SerialGC, `-XX:+UseCompressedOops` |
| Графика | NXVK (Vulkan, форк Mesa NVK) + Zink → OpenGL для LWJGL 3.3.3 |
| Игра | Minecraft 1.21.1 + Fabric + Sodium: мир, генерация, сохранение; ~87 FPS в полёте над миром |
| Память | Java-куча до **1900 МиБ** (стандартный бюджет процесса), mimalloc 2.2.4 с общим пулом страниц и алиасингом GPU-буферов |
| CPU | 4 ядра через NSP-forwarder (ядро 3 разблокировано в NPDM) |
| Запуск | ~55 с до игры, ~63 с до мира (кэш JAR-файлов целиком, `CICompilerCount=3`) |
| Лаунчер | Сборки, мастер версий/загрузчиков, моды/ресурспаки/шейдеры из Modrinth и CurseForge с зависимостями и обновлениями, миры, серверы, скриншоты, логи, импорт `.mrpack` / CurseForge ZIP / экспорт MultiMC-PolyMC-Prism, packwiz |

Ещё не проверено или не сделано:

- **NeoForge/Forge** — загрузчики поддерживаются лаунчером, но на консоли пока не гонялись.
- **Java 17 и Java 8** собраны и слинкованы ([JAVA17_PORT](docs/JAVA17_PORT.md),
  [JAVA8_PORT](docs/JAVA8_PORT.md)), на устройстве не тестировались и в лаунчер не встроены.
- Вход через Microsoft-аккаунт не реализован: только офлайн-профили.
- Моды с нативными библиотеками для ПК (голосовой чат, видеоплееры и т. п.) работать не будут.

## Требования

- Nintendo Switch с Atmosphère (проверено на OLED). Нужен **режим приложения**
  (полная память): запуск через NSP-forwarder или hbmenu с удержанием R над игрой.
  В режиме апплета (альбом) памяти не хватит.
- Около 3 ГБ свободного места на SD-карте плюс место под сборки.
- Своя лицензия Minecraft: Java Edition. Проект **не распространяет** код и ресурсы Mojang;
  лаунчер скачивает их с официальных серверов Mojang на консоли.

## Установка

1. Скопировать `MineSwitch.nro` в `sdmc:/switch/mineswitch/MineSwitch.nro`.
2. Запустить из hbmenu с удержанием **R** над любой игрой (полная память),
   либо собрать и установить NSP-forwarder (см. ниже) — тогда MineSwitch появится
   в HOME и получит 4 ядра и +96 МиБ памяти.
3. В лаунчере: «Сборки» → создать сборку (версия + загрузчик) → «Играть».
   Готовые сборки: «Каталог» (Modrinth/CurseForge) или «Импорт с SD»
   (архивы класть в `sdmc:/switch/mineswitch/imports/`).

Управление лаунчером: [launcher/README.md](launcher/README.md).

## Как это устроено

```
MineSwitch.nro
├── лаунчер (SDL2 + FreeType, C++17)          launcher/
│   каталоги, импорт, подготовка сборки, packwiz
└── рантайм                                    runtime/launcher/
    ├── OpenJDK 21 HotSpot, статически         runtime/patches/openjdk21-horizon.patch
    ├── нативные библиотеки JDK и LWJGL        runtime/lwjgl/, runtime/glfw/
    ├── NXVK/Zink                              runtime/graphics/nxvk/
    ├── mimalloc для Horizon                   runtime/memory/mimalloc/
    ├── файловый ввод-вывод, кэш JAR           runtime/io/
    └── JDK-образ в RomFS                      runtime/build-image.sh
```

| Каталог | Что внутри |
| --- | --- |
| `launcher/` | Лаунчер: интерфейс, хранилище, каталоги, импорт, моды, packwiz, тесты для ПК |
| `runtime/` | Порт OpenJDK (патчи, сборочные скрипты), загрузчик JVM, графика, память, ввод-вывод |
| `runtime/patches/` | Патчи OpenJDK 21/17/8 для Horizon (применяются к закреплённым коммитам upstream) |
| `runtime/forwarder/` | Сборка NSP-forwarder (нужны собственные ключи консоли) |
| `runtime/maintenance/` | Служебное NRO для заливки сборок по сети при разработке |
| `minecraft/` | Подготовка запуска Minecraft и политика модулей JDK |
| `diagnostic/`, `graphics/`, `vm-probe/` | Небольшие диагностические NRO первых этапов |
| `docs/` | Журнал портирования и результаты проверок на устройстве |
| `scripts/` | Тулчейн, запуск NRO через NXLink, анализ логов |

## Сборка

Нужны Linux x86_64, Python 3.12+, GNU Make, JDK 21 на хосте (boot JDK) и
~15 ГБ места. Все сборки идут через `runtime/limit-build.py` (по умолчанию
6 потоков, nice 15).

1. **Тулчейн** devkitA64/libnx (закреплённый слой образа devkitPro, проверка SHA-256):
   ```sh
   python3 scripts/bootstrap-toolchain.py
   ```
2. **OpenJDK 21** — исходники закреплённого коммита + патч Horizon, затем HotSpot server
   и нативные библиотеки ([runtime/README.md](runtime/README.md),
   [runtime/JIT_BUILD.md](runtime/JIT_BUILD.md)):
   ```sh
   python3 runtime/bootstrap-openjdk.py
   export MINESWITCH_JVM_VARIANT=server MINESWITCH_JDK_CONF=horizon-aarch64-server-release
   bash runtime/configure-horizon.sh
   bash runtime/build-horizon.sh hotspot
   bash runtime/build-horizon.sh java.base-libs
   ```
3. **Зависимости рантайма**: mimalloc (`runtime/memory/mimalloc/build.py`),
   NXVK (`runtime/graphics/nxvk/configure-local.py`), LWJGL
   (`runtime/lwjgl/build_native.py`, `build_java.py`), GLFW (`runtime/glfw/`).
4. **JDK-образ и реестр нативных символов**:
   `runtime/build-jit-image.sh` → `runtime/generated-native-minecraft/`.
5. **Итоговое NRO** (лаунчер + рантайм):
   ```sh
   python3 runtime/limit-build.py -- make -C runtime/launcher -j6 \
     DEVKITPRO=$PWD/.toolchain/opt/devkitpro DEVKITA64=$PWD/.toolchain/opt/devkitpro/devkitA64 \
     REGISTRY=$PWD/runtime/generated-native-minecraft/native_registry.c \
     ROMFS=$PWD/runtime/generated-native-minecraft/romfs \
     APP_MODE=launcher GRAPHICS_BACKEND=nxvk ALLOCATOR_BACKEND=mimalloc \
     ALLOCATOR=$PWD/runtime/memory/mimalloc/build-s18/libmineswitch_mimalloc.a \
     PROFILE_ALLOC=1 GPU_ALIAS=1 RECLAIM_NXVK=1 COMPACT_VM=1 \
     BUILD=$PWD/runtime/launcher/build OUTPUT=$PWD/dist/MineSwitch
   ```
6. **NSP-forwarder** (по желанию; ключи не входят в репозиторий и в результат):
   ```sh
   python3 runtime/forwarder/build.py --keys /path/to/prod.keys
   ```

Полная сборка с нуля ещё не сведена в один скрипт; шаги 3–4 описаны подробнее
в [docs/PORT_PROGRESS.md](docs/PORT_PROGRESS.md) и документах по компонентам.

Лаунчер отдельно собирается и тестируется на ПК (SDL2, FreeType, Jansson, libcurl,
minizip, libwebp, mbedTLS):

```sh
python3 runtime/limit-build.py -- make -C launcher -j6 check ui-check
```

## Документы

- [Порт Java 21 на Horizon](docs/JAVA21_PORT.md) и [запуск JIT](docs/JIT_BRINGUP.md)
- [Ход портирования](docs/PORT_PROGRESS.md), [результаты на устройстве](docs/DEVICE_BASELINE.md)
- Память: [mimalloc на Horizon](docs/MIMALLOC_HORIZON.md), [память NXVK](docs/NXVK_MEMORY.md),
  [JIT и память Horizon](docs/HORIZON_JIT_MEMORY.md)
- Особенности Horizon: [стековые guard-страницы](docs/HORIZON_STACK_GUARDS.md),
  [планирование потоков](docs/HORIZON_THREAD_SCHEDULING.md), [завершение процесса](docs/HORIZON_PROCESS_EXIT.md)
- Лаунчер: [моды](docs/MOD_MANAGEMENT.md), [геймпады](docs/LAUNCHER_CONTROLLERS.md),
  [проверка](docs/LAUNCHER_VALIDATION.md), [анализ запуска игры](docs/MINECRAFT_STARTUP_ANALYSIS.md)

## Лицензия и правовое

Код проекта распространяется под [GPL-2.0](LICENSE).

MineSwitch — неофициальный проект, не связанный с Mojang, Microsoft или Nintendo.
Minecraft — товарный знак Mojang Synergies AB. Репозиторий не содержит кода,
ресурсов или ключей Mojang и Nintendo.

Сторонний код: OpenJDK (GPLv2 + Classpath Exception; здесь только патчи),
Mesa (MIT), mimalloc (MIT), LWJGL (BSD-3), GLFW (zlib), newlib, stb_image (public domain),
nx-hbloader/NTON (ISC). Upstream-исходники скачиваются сборочными скриптами
по закреплённым версиям.
