# Java 17 для Horizon: порт OpenJDK 17u (нативный AArch64 HotSpot C1/C2)

Дата: 25 октября 2026. Цель — Minecraft Java 1.17–1.20.4 (Mojang требует
Java 16/17; 1.17 штатно идёт на Java 17). Порт выполнен по образцу рабочего
порта OpenJDK 21 (`docs/JAVA21_PORT.md`, `docs/JIT_BRINGUP.md`) и не меняет ни
дерево `.upstream/openjdk21`, ни его патч, ни скрипты Java 21, ни `launcher/`,
`runtime/launcher/`, `dist/`.

## Статус одной таблицей

| Этап | Результат | Уровень доказательства |
| --- | --- | --- |
| Исходники 17u закреплены, патч Horizon экспортирован | `jdk-17.0.19-ga`, 180 файлов / +8912 −177 строк | проверено `git apply --check` на чистой базе |
| `configure` для `aarch64-unknown-horizon` | features `compiler1 compiler2 jni-check management nmt serialgc static-build` | **собрано** |
| HotSpot 17 (`libjvm.a`, server, C1+C2, ADLC AArch64) | 584 MB статический архив | **собрано** |
| Нативные библиотеки JDK (22 модуля MC-профиля, 20 архивов) | java.base, java.desktop (headless), management, prefs, rmi… | **собрано** |
| Образ runtime (jlink, сжатый) | 22 модуля, 16 105 классов, `jimage verify` проходит | **собрано**, проверено host-`jimage` |
| NRO-пробник JVM (без графики) | `--no-undefined`, 9 библиотек, 829 функций в реестре | **слинковано** |
| NRO JVM + LWJGL 3.3.3/GLFW/EGL/STB/OpenAL | 26 библиотек, 5845 функций в реестре | **слинковано** |
| WhiteBox JIT-пробы и native-contract пробы (те же исходники, что для 21) | host JDK 17.0.19: JIT 51/51 и contract 31/31 на уровнях C1 и C2 | **host-tested** (проверяет пробы, не порт) |
| Запуск JVM на Switch, C1/C2 на устройстве, LWJGL-кадры, Minecraft | — | **нужен тест на устройстве** |

Ни одна команда на Switch не выполнялась. Ничего не загружалось на устройство.

## Закрепление исходников

| Поле | Значение |
| --- | --- |
| Репозиторий | https://github.com/openjdk/jdk17u |
| Тег | `jdk-17.0.19-ga` (совпадает с host Temurin 17.0.19, который служит Boot/Build JDK) |
| Коммит | `ad45f7b18fa661b479f691f598b024107584be1e` |
| SHA256 архива codeload | `b62af1012b5aa650ebc220a1f0fe69aa885714c9c581b743e89f5c4b04b4afaf` |
| Запись | `.upstream/OPENJDK17_SOURCE.json`, архив `.upstream/jdk17u-ad45f7b…tar.gz` |
| Дерево | `.upstream/openjdk17` (git-база = импорт тега) |
| Патч | `runtime/patches/openjdk17-horizon.patch` (SHA256 `5de762db4f1d31e2d3641f3de2b5ce9a3bd40e184711dbc99d8850b6f7508031`) |

Воспроизведение с нуля: `python3 runtime/java17/bootstrap-openjdk17.py`
(скачивает, проверяет SHA256, создаёт git-базу, применяет патч). После правок
в дереве: `python3 runtime/java17/export-openjdk17-patch.py`.

## Команды сборки

Все команды сами проходят через `runtime/limit-build.py` (6 потоков, nice 15,
общий lock), Boot/Build JDK — `/opt/openjdk-bin-17`.

```sh
bash runtime/java17/configure-horizon-17.sh            # build/horizon-aarch64-server-release
bash runtime/java17/build-horizon-17.sh hotspot        # libjvm.a
# Пробный образ (java.base + management) и пробы:
MINESWITCH_JDK17_GENERATED_DIR=$PWD/runtime/java17/generated-probe \
MINESWITCH_MODULES=java.base,java.management,jdk.management MINESWITCH_NATIVE_SPECS=none \
  bash runtime/java17/build-image-17.sh
python3 runtime/java17/tests/prepare-17.py              # HelloHorizon + JIT-пробы в ROMFS
python3 runtime/java17/tests/prepare-17.py --host-check # те же пробы на host JDK 17
python3 runtime/limit-build.py -- make -C runtime/java17/launcher -j6
# Полный профиль Minecraft + реестр LWJGL (по умолчанию runtime/java17/generated):
bash runtime/java17/build-image-17.sh
python3 runtime/java17/tests/prepare-lwjgl-17.py
python3 runtime/limit-build.py -- make -C runtime/java17/launcher -j6 GRAPHICS=1 \
  GENERATED=$PWD/runtime/java17/generated BUILD=$PWD/runtime/java17/launcher/build-lwjgl \
  OUTPUT=$PWD/runtime/java17/launcher/mineswitch-java17-lwjgl
```

Профиль модулей: `runtime/java17/modules-minecraft-java17.txt` (те же корни, что
у проверенного 1.21.1; замыкание — 22 модуля). Его стоит перепроверить
`runtime/analyze-minecraft-modules.py` на реальных jar 1.18.2/1.20.1.

## Артефакты

| Путь | Назначение | SHA256 |
| --- | --- | --- |
| `runtime/java17/launcher/mineswitch-java17-probe.nro` (24.7 MB) | JVM-пробник: HelloHorizon, WhiteBox JIT C1/C2, native contracts | `23fc1171906fc760a6512afac97639bf7ec7c3062fa16ba815e44f8bdc5b9039` |
| `runtime/java17/launcher/mineswitch-java17-lwjgl.nro` (56.3 MB) | полный образ (22 модуля) + LWJGL/GLFW/GL/STB/OpenAL, LwjglProbe | `e1664a88be400b33e554a941d6bb5956a16a9c68ef84a9c1fe1d0aef6b56aa4f` |
| `runtime/java17/generated-probe/` | ROMFS пробника (`jdk/`, `tests/`, `launch/`), реестр, `native-archives.mk` | — |
| `runtime/java17/generated/` | ROMFS полного runtime-17 (114 файлов образа + 5 jar пробы LWJGL), реестр 26 библиотек | — |
| `.upstream/openjdk17/build/horizon-aarch64-server-release/support/modules_libs/**.a` | статические архивы JDK 17 | — |
| `logs/jdk17/*.log` | журналы configure/build/link и `jit-probe-host.log` | — |

`.elf` и `.map` рядом с NRO сохранены для символизации будущих аварийных логов.

## Что сделано в порте (отличия 17 от 21)

Слой ОС взят из Java 21 и переписан под API HotSpot 17:

* **Заголовки внутри `class os`.** В 17 `os_<os>.hpp` включается в тело класса
  `os` вместо `os_posix.hpp`. Новый `os/horizon/os_horizon.hpp` объявляет
  разделители путей (`;` из-за `sdmc:`), минимальные размеры стеков,
  `zero_page_read_protected`, `PlatformEvent/Parker/Mutex/Monitor` и
  `ThreadCrashProtection` как вложенные классы `os::`; C-мост вынесен в
  `os_horizon_impl.hpp`. `share/runtime/os.hpp` не включает `os_posix.hpp` для
  Horizon, `semaphore.hpp` выбирает `semaphore_horizon.hpp`, шаги vmError,
  требующие `os::Posix`, отключены для Horizon.
* **API 17:** `OSThread(start, parm)`, `julong physical/available_memory`,
  `os::vm_page_size()` вместо `OSInfo`, `os::open(int, mode)` вместо `fdopen`,
  `check_dump_limit`, `pause`, `available`, `fsync`, `close`, `socket`,
  `print_tos_pc`, `print_register_info` без продолжения, `as_Java_thread()`,
  `Atomic::PlatformAdd::{add,fetch}_and_*`, `thread_horizon_aarch64.*` вместо
  `javaThread_*`, `bytes_/os_horizon_aarch64.hpp`, `os::SuspendedThreadTask`.
* **Деоптимизация.** В 17 нет post-call NOP (Loom): обработчик исключений
  Horizon не перехватывает `NativeDeoptInstruction`; not-entrant помечается
  `is_sigill_zombie_not_entrant`. Стековые guard-зоны, polling page, неявные
  null-проверки, JNI fast-accessors — та же логика, что проверена на 21.
* **RW/RX CodeMemory.** Повторно применены и проверены вручную все места
  записи в код (CodeSection `emit_*`, включая 17-специфичные `emit_int16/32/64`,
  `emit_float/double/address`; relocate/fill в CodeBuffer; CodeBlob/nmethod
  RX-границы; Stub/ICStub/InterpreterCodelet; VtableStub; сигнатурные handlers;
  CodeHeap::contains/find_block_for; ReservedCodeSpace через
  `reserve_memory_special`; патчинг инструкций и GOT/literal ячеек AArch64).
  C1 на AArch64 в 17 не патчит код (deoptimize_trap). Поиск прямых записей в
  `cpu/aarch64` других мест не нашёл.
* **JDK-библиотеки.** Горизонтовые override-файлы Java 21 сверены с JNI-
  сигнатурами 17 (автоматическая сверка 166 функций с javac-заголовками — 0
  расхождений). Переписаны под 17: `UnixNativeDispatcher.c` (`stat0/stat1/
  exists0/access0`, имена без `0`), `FileDispatcherImpl.c` и `.java` (в 17
  нет `UnixFileDispatcherImpl/UnixDispatcher`), `FileChannelImpl.c` (map
  недоступен, transferTo → Java-цикл), `SocketDispatcher.java` (сокеты пишут
  через send, а не через файловый мост), `NativeThread.c`, `Console_md.c`,
  `Inet6AddressImpl.c`, `java_props_md.c`. Добавлен Horizon-only
  `jdk.internal.util.OperatingSystem` (в 17 его нет). Устаревшие
  `PlainSocketImpl/PlainDatagramSocketImpl/Socket*Stream` natives исключены
  (используются только с `-Djdk.net.usePlain*Impl`), `NET_SocketClose/Connect/
  Poll` — простые обёртки без сигнального прерывания. В статической сборке
  исправлены коллизии `jvm/jmm_interface/jmm_version/throw_internal_error`
  между libmanagement и libmanagement_ext (как в 21). `libfdlibm.a`
  (StrictMath в 17 ещё нативный) линкуется явно. Шрифтовой менеджер AWT
  явно не поддерживается (как в 21).
* **Сборка.** `jmod --date` в 17 нет; host `jlink` 17 принимает
  `horizon-aarch64` как UNKNOWN без патча enum. Реестр natives без
  `libsyslookup`.

## Ограничения и риски (честно)

* На устройстве **ничего не проверено**: создание VM, guard-страницы стеков,
  C1/C2 на CodeMemory, GC, исключения ядра — всё это доказано только для 21.
  Общий код Horizon идентичен, но HotSpot 17 устроен иначе в деоптимизации и
  стеках; первые тесты должны идти по плану ниже.
* `FileChannel.map`, direct I/O, `transferTo` через sendfile, IPv6-lookup,
  POSIX-сигналы, процессы, JFR, CDS, JVMTI недоступны (как в 21).
* По умолчанию пробник задаёт `-XX:ReservedCodeCacheSize=64m`: CodeMemory
  закрепляет всю резервацию сразу; дефолт 17 (240 MiB) слишком дорог.
* `runtime/memory`, `runtime/io`, `runtime/safefetch` — общие с Java 21
  исходники (компилируются в обе JVM). Их правки затрагивают оба порта.
* Профиль модулей не перепроверен на jar 1.17–1.20.4.

## План проверки на устройстве (выполняет владелец)

Команды запускаются с ПК; IP — адрес Switch. Каждый запуск ставит/проверяет
`sdmc:/switch/mineswitch/runtime-17/` по `files.manifest` (Java 21 runtime
не затрагивается). После каждого запуска Sphaira нужно открыть снова.

1. Базовая JVM: `bash scripts/run-nro.sh IP runtime/java17/launcher/mineswitch-java17-probe.nro`
   — ожидаются `JNI_CREATE_END result=0`, `java.version=17.0.19-internal`,
   `MINESWITCH_JAVA_PASS checks=54`, `JNI_DESTROY_END result=0`,
   `MINESWITCH_JVM_END result=0`.
2. C1: `… mineswitch-java17-probe.nro --system-main --class=mineswitch.probe.JitProbe --jvm-options=romfs:/launch/jit-level1.options -- 1`
   — `MINESWITCH_JIT_PASS checks=51 level=1`.
3. C2: то же с `jit-level4.options -- 4` — `checks=51 level=4`.
4. Контракты: `--class=mineswitch.probe.NativeContractProbe` с теми же опциями
   `-- 1` и `-- 4` — `MINESWITCH_NATIVE_CONTRACT_PASS checks=31`.
5. Графика: `bash scripts/run-nro.sh IP runtime/java17/launcher/mineswitch-java17-lwjgl.nro --graphics --system-main --class=mineswitch.probe.LwjglProbe --jvm-options=romfs:/launch/lwjgl-probe.options`
   — `MINESWITCH_LWJGL_PASS … frames=90` (как 24 проверки/90 кадров на 21).
6. После интеграции в лаунчер: ванильный 1.20.1, затем 1.18.2 и 1.17.1,
   офлайн-мир, сохранение/повторное открытие; Fabric для 1.20.1.

При сбое прислать полный NXLink-лог и `sdmc:/switch/mineswitch/hs_err_pid*.log`.

## Контракт интеграции с лаунчером (не реализован, только описание)

**Раскладка SD.**

```
sdmc:/switch/mineswitch/
  runtime/        Java 21 (как сейчас)
  runtime-17/     Java 17: ставится из romfs:/jdk/ NRO Java 17 по files.manifest
  runtime-8/      Java 8: JRE-раскладка (rt.jar, horizon.jar), см. JAVA8_PORT.md
  mineswitch.nro  фронтенд + JVM 21 (текущий)
  mineswitch-java17.nro   JVM 17 + LWJGL + код запуска игры (без UI каталога)
```

`MINESWITCH_JAVA_HOME`/`-Djava.home` = `/switch/mineswitch/runtime-17`; путь
по умолчанию зашит и в `os_horizon.cpp`. Временный каталог и `user.home` —
общие (`/switch/mineswitch/tmp`, `/switch/mineswitch`).

**Выбор версии Java.** Поле `javaVersion.majorVersion` из JSON версии Mojang:
16 и 17 → Java 17; 21 → Java 21; 8 → Java 8 (`mineswitch-java8.nro`, см.
`docs/JAVA8_PORT.md`; ванильные 1.13–1.16.5 можно пробовать и на 17). Сохранять выбор в метаданных инстанса (`javaMajor`), с
возможностью ручного переопределения.

**Один NRO или несколько — рекомендация: отдельный NRO на каждую версию JVM.**
Две статические HotSpot в одном ELF не совместимы без глубокой переделки:
одинаковые глобальные C++/JNI символы (`JNI_CreateJavaVM`, `Java_*` с разной
реализацией для 17 и 21), один `__libnx_exception_entry`, один реестр
`horizon_register_native_library`, один malloc-оверлей. Кроме того, весь код
NRO резидентен: вторая JVM с LWJGL — это десятки МБ памяти, отнятые у кучи
Minecraft, а пересоздать уничтоженную VM в процессе HotSpot не умеет.

**Переход между NRO.** Фронтенд (текущий `mineswitch.nro`) выбирает инстанс до
создания JVM. Если нужен Java 17, он должен: записать передачу
`sdmc:/switch/mineswitch/launch/next.json` (id инстанса, файл опций, рабочий
каталог, main class, аргументы), вызвать
`envSetNextLoad("sdmc:/switch/mineswitch/mineswitch-java17.nro", "mineswitch-java17.nro --instance=<id>")`
и **обычным возвратом из `main`** вернуться в hbloader (не через
`horizon_exit_process`: ISelfController::Exit завершает весь процесс и
следующая загрузка не произойдёт). Это безопасно только пока не создана JVM и
нет её потоков. Проверить на устройстве `envHasNextLoad()`.
`mineswitch-java17.nro` читает `next.json`, ставит `runtime-17`, создаёт JVM 17,
запускает игру и завершается тем же путём, что и 21 (`horizon_exit_process`).

**Что нужно поменять в лаунчере.**

* Сборка второго варианта NRO: те же исходники `runtime/launcher` с макросами
  `MINESWITCH_RUNTIME_HOME`/`RUNTIME_TARGET` (`runtime-17`), реестром и
  архивами из `runtime/java17/generated/native-archives.mk` (включая
  `MINESWITCH_SUPPORT_ARCHIVES` = `libfdlibm.a`), JDK-заголовками 17. Готовый
  образец — `runtime/java17/launcher/Makefile` (режим `GRAPHICS=1`).
* Java-помощники (`ClientEntry`, `mineswitch-compat.jar`, `launcher-tools.jar`,
  `minecraft-bootstrap.jar`, `ProgressSampler`) собирать с `--release 17`
  (сейчас `minecraft/prepare-launch.py` использует `--release 21`).
* Опции JVM — отдельные файлы на версию: флаги, неизвестные 17, приведут к
  отказу `JNI_CreateJavaVM` (`ignoreUnrecognized=false`). Для 17 обязательны
  `-Xrs`, `-XX:-UsePerfData`, `-XX:+UseSerialGC`, явный
  `-XX:ReservedCodeCacheSize` (64–128m).
* LWJGL: те же Horizon-jar 3.3.3 (`--release 8`) и нативы, что у 21. Minecraft
  1.17–1.18 рассчитан на LWJGL 3.2.x, 1.19–1.20.4 — на 3.3.1/3.3.2; замена на
  3.3.3 — штатная практика (ARM-Mac), но требует проверки на каждой версии.
* Политика boot-модулей (исключение JFR, `minecraft/MODULE_POLICY.md`) та же.

## Следующий шаг

Провести пункты 1–5 плана на устройстве. При успехе — передать агенту
лаунчера контракт выше (второй NRO + передача через `envSetNextLoad`) и
проверить ванильный 1.20.1.
