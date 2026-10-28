# Java 8 для Horizon: порт OpenJDK 8u (нативный AArch64 HotSpot C1/C2)

Дата: 25 октября 2026. Цель — Minecraft Java ≤ 1.16.5 (официально Java 8) и
прежде всего старые Forge/LaunchWrapper-сборки, которым нужен именно Java 8.
Порт сделан по образцу портов OpenJDK 21 (`docs/JAVA21_PORT.md`) и 17
(`docs/JAVA17_PORT.md`) и не меняет ни деревья/патчи 21 и 17, ни их скрипты, ни
`launcher/`, `runtime/launcher/`, `dist/`.

## Оценка осуществимости (честно)

JDK 8 устроен заметно иначе, чем 17/21, но порт **осуществим и в основной части
уже собран**:

* HotSpot 8 собирается отдельной make-системой (`hotspot/make`), без
  `configure` верхнего уровня. Добавлена ОС `horizon` (`OSNAME=horizon`,
  `hotspot/make/horizon`), статическая сборка `libjvm.a`. Платформенная
  диспетчеризация 8 — через `TARGET_OS_FAMILY_*`/`TARGET_OS_ARCH_*` в общем
  коде (52 блока дополнены веткой Horizon). Код C++98.
* Верхнеуровневая сборка JDK 8 не умеет ни статические библиотеки, ни новую ОС;
  поэтому нативные библиотеки JDK собираются собственным скриптом с теми же
  наборами исходников, что `jdk/make/lib/*.gmk` для Linux, плюс Horizon-override.
  Классы (`rt.jar` и т.д.) не пересобираются: берутся побайтно из закреплённого
  Temurin 8u492-b09 (тот же тег `jdk8u492-ga`), платформенные отличия — в
  отдельном `lib/horizon.jar`, который HotSpot ставит первым в boot class path.
* Статические JNI-библиотеки: в 8 есть JEP 178 (`JNI_OnLoad_<lib>`, требование
  JNI 1.8 для встроенных библиотек) — тот же механизм реестра, что в 17/21.
* Главный незакрытый объём для Minecraft ≤ 1.12.2 — не JVM, а **AWT/Java2D
  (headless)** и **LWJGL 2** (см. ниже). Для 1.13–1.16.5 нужен LWJGL 3.2.x —
  его закрывает уже существующий Horizon-порт LWJGL 3.3.3 (собран NRO).

## Статус одной таблицей

| Этап | Результат | Уровень доказательства |
| --- | --- | --- |
| Исходники 8u закреплены, патч Horizon экспортирован | `jdk8u492-ga`, 200 файлов / +13006 −78 строк | `git apply --check` на чистой базе — проходит |
| HotSpot 8 (`libjvm.a`, server, tiered C1+C2, ADLC AArch64) | 30.7 MB статический архив | **собрано** |
| Слой ОС Horizon в HotSpot 8 (потоки, память, CodeMemory RW/RX, исключения, стековые guard-зоны) | перенесён с 21/17 под API 8 | **собрано**, на устройстве не проверено |
| Нативные библиотеки JDK 8: fdlibm, verify, java, zip, nio, net, management | 7 статических архивов (62/2/65/21/17/19/15 объектов) | **собрано** |
| Платформенные классы `lib/horizon.jar` | 19 классов (NIO FS provider, SocketDispatcher, процессы, seed, os.name-диспетчеры) | **собрано**, **host-tested** 15/15 (`logs/jdk8/overlay-host-check.log`) |
| JRE-раскладка runtime-8 (ROMFS → `sdmc:/switch/mineswitch/runtime-8`) | 32 файла, 80.5 MB, `files.manifest` с SHA256 | **собрано** |
| NRO-пробник JVM (без графики) | `--no-undefined`, 7 библиотек, 867 функций в реестре | **слинковано** |
| NRO JVM 8 + LWJGL 3.3.3/GLFW/EGL/STB/OpenAL | 13 библиотек, 5388 функций в реестре | **слинковано** |
| Наборы опций для разбора JIT (`-Xint`, eager C1, eager C2) | флаги приняты host HotSpot 8u492 | **host-checked** (только синтаксис флагов) |
| libawt/Java2D headless, шрифты, звук (javax.sound), LWJGL 2 | — | **не сделано** |
| Запуск JVM на Switch, C1/C2 на устройстве, LWJGL-кадры, Minecraft | — | **нужен тест на устройстве** |

Ни одна команда на Switch не выполнялась. Ничего не загружалось на устройство.

## Закрепление исходников и класс-библиотеки

| Поле | Значение |
| --- | --- |
| Репозиторий | https://github.com/openjdk/jdk8u |
| Тег | `jdk8u492-ga` |
| Коммит | `9cecb1477fff90f2d367a4df0a94cb44510d1ba9` |
| SHA256 архива codeload | `8e44d6a9c04f590f3f0c6a6b95a74733ba8c347f4c50993dea7d68e06861ff1e` |
| Запись | `.upstream/OPENJDK8_SOURCE.json`, архив `.upstream/jdk8u-9cecb14…tar.gz` |
| Дерево | `.upstream/openjdk8` (git-база `5571e03d` = импорт тега) |
| Патч | `runtime/patches/openjdk8-horizon.patch` (SHA256 `b87462d5b5a5af1520e6b59de038d03b661ee3287f5cad3eef34be44a8541508`) |
| Класс-библиотека | Temurin `1.8.0_492-b09` (`/opt/openjdk-bin-8` → `openjdk-bin-8.492_p09`), тот же тег 8u492; SHA256 30 файлов закреплены в `runtime/java8/jre-8u492-files.json` (`lib/rt.jar` = `4183642f486186ac639862f921e74531da5e243b5b494dd7e2195f353a6c0bd6`); `prepare-runtime-8.py` отказывается работать при расхождении |

Воспроизведение с нуля: `python3 runtime/java8/bootstrap-openjdk8.py`
(скачивает, проверяет SHA256, создаёт git-базу, применяет патч). После правок
в дереве: `python3 runtime/java8/export-openjdk8-patch.py` (экспорт и проверка
применимости к базе; `--check` — только проверка).

## Команды сборки

Компиляция идёт через `runtime/limit-build.py` (6 потоков, nice 15, общий
lock): скрипты сами перезапускаются через него; `make` вызывается явно.
Boot JDK и класс-библиотека — `/opt/openjdk-bin-8` (Temurin 8u492-b09).

```sh
python3 runtime/java8/bootstrap-openjdk8.py               # дерево + патч (идемпотентно)
bash runtime/java8/build-hotspot-8.sh                      # libjvm.a (product)
python3 runtime/java8/tools/gen-jni-headers-8.py \
  --rt-jar /opt/openjdk-bin-8/jre/lib/rt.jar --javah /opt/openjdk-bin-8/bin/javah \
  --output .upstream/openjdk8/build/horizon-aarch64-jdk/headers   # JNI-заголовки из rt.jar
python3 runtime/java8/build-jdk-natives-8.py               # fdlibm verify java zip net nio management
python3 runtime/java8/prepare-runtime-8.py                 # ROMFS: JRE + horizon.jar + тест + опции
python3 runtime/java8/generate-native-registry-8.py        # реестр 7 библиотек
python3 runtime/limit-build.py -- make -C runtime/java8/launcher -j6
# Графический вариант (LWJGL 3.3.3 Horizon):
MINESWITCH_JDK8_GENERATED_DIR=$PWD/runtime/java8/generated-lwjgl python3 runtime/java8/prepare-runtime-8.py
python3 runtime/java8/tests/prepare-lwjgl-8.py
python3 runtime/java8/generate-native-registry-8.py --lwjgl --output-dir runtime/java8/generated-lwjgl
python3 runtime/limit-build.py -- make -C runtime/java8/launcher -j6 GRAPHICS=1 \
  GENERATED=$PWD/runtime/java8/generated-lwjgl BUILD=$PWD/runtime/java8/launcher/build-lwjgl \
  OUTPUT=$PWD/runtime/java8/launcher/mineswitch-java8-lwjgl
# Host-проверка платформенных классов на host JDK 8:
javac -d /tmp/oc runtime/java8/tests/OverlayHostCheck.java
java -Xbootclasspath/p:runtime/java8/generated-probe/romfs/jdk/lib/horizon.jar \
  -Dos.name=Horizon -cp /tmp/oc OverlayHostCheck /tmp/oc-data   # OVERLAY_HOST_PASS checks=15
```

`build-hotspot-8.sh` передаёт `OSNAME=horizon`, кросс-компилятор devkitA64 GCC
15.2, `INCLUDE_JVMTI/FPROF/VM_STRUCTS/SERVICES/CDS=false`, JFR выключен
(management включён). Host-ADLC собирается с Linux-определениями.

## Артефакты

| Путь | Назначение | SHA256 |
| --- | --- | --- |
| `runtime/java8/launcher/mineswitch-java8-probe.nro` (91.1 MB) | JVM-пробник: HelloHorizon (46 проверок: VM, GC, потоки, JNI, rt.jar, zip, fdlibm, java.io, NIO-файлы и map, сокеты, селектор, MXBeans, SecureRandom) | `8862dd829a896d1b764f558049a82a703e689ee84e575cb104b2be7033c7d463` |
| `runtime/java8/launcher/mineswitch-java8-lwjgl.nro` (100.8 MB) | то же + LWJGL/GLFW/GL/STB/OpenAL, LwjglProbe | `91836a82a44a209125b3a1b1a4c23138fea263e536d813c81c90c3d691ac9d1f` |
| `runtime/java8/generated-probe/`, `runtime/java8/generated-lwjgl/` | ROMFS (`jdk/`, `tests/`, `launch/`), `native_registry.c/.json`, `native-archives.mk` | — |
| `.upstream/openjdk8/build/horizon-aarch64-hotspot/horizon_aarch64_compiler2/product/libjvm.a` | HotSpot 8 | — |
| `.upstream/openjdk8/build/horizon-aarch64-jdk/lib/lib*.a` | нативные библиотеки JDK 8 | — |
| `logs/jdk8/*.log` | журналы сборок, линковки, host-проверки | — |

NRO большие из-за ROMFS (JRE 80 MB: `rt.jar` 63 MB, локали, charsets). При
первом запуске JRE ставится на SD по манифесту; в лаунчере ROMFS можно не
встраивать, а ставить `runtime-8` отдельно (см. контракт).

## Что сделано в порте

**HotSpot 8.**

* `hotspot/make/horizon/` (копия linux-make): платформа `horizon_aarch64`,
  `-DHORIZON -D__SWITCH__ -DAARCH64`, `-std=gnu++98`, статический `libjvm.a`
  через `ar rcs`, без dtrace/jsig/SA, исходники `runtime/memory` и
  `runtime/safefetch` (общие с 21/17), правила для `.c`/`.S`.
* `os/horizon/vm/` и `os_cpu/horizon_aarch64/vm/`: потоки libnx + reaper без
  detached-потоков, TLS, время/сон/прерывание, nxvm-память с реальными
  правами страниц, polling page, статический реестр библиотек
  (`dll_load/dll_lookup`), файлы через nxio, свойства (`java.home` по
  умолчанию `/switch/mineswitch/runtime-8`, разделитель путей `;`, boot path
  `lib/horizon.jar;lib/resources.jar;lib/rt.jar;…`), обработчик исключений
  ядра (safefetch, yellow/red zone, zombie, polling, неявные null-проверки,
  JNI fast get field), сигналы не поддерживаются (`-Xrs` обязателен).
* **RW/RX CodeMemory** перенесён в API 8: CodeBuffer/CodeSection, assembler,
  CodeBlob/nmethod RX-границы, Stub/ICStub/InterpreterCodelet, VtableStub,
  CodeHeap, relocInfo, патчинг инструкций AArch64 (`nativeInst`,
  `relocInfo_aarch64`), сброс I-cache через nxjit, ReservedCodeSpace через
  `reserve_memory_special`.
* `vm_version_aarch64`: фиксированные HWCAP Cortex-A57 вместо auxv/`/proc`.

**Нативные библиотеки JDK** (`runtime/java8/build-jdk-natives-8.py`,
override-файлы в `jdk/src/horizon/native/<lib>/`):

* libjava: nxio-ввод/вывод (`io_util_md`, `UnixFileSystem_md`, canonicalize),
  `java_props_md` (os.name `Horizon`, UTF-8), TimeZone, Console, процессы
  (`IOException`), CSRNG-seed (`HorizonSeedGenerator.c`), встроенные
  библиотеки без `JNI_OnLoad` (`ClassLoader.c`), `check_version` → JNI 1.8,
  `verify_stub.c` (VerifyClassCodes* для HotSpot).
* libnio: портированы Horizon-версии из 17 (IOUtil с nxio-loopback pipe,
  FileDispatcherImpl, SocketDispatcher через send/recv, DatagramDispatcher,
  FileKey по каноническому пути, UnixNativeDispatcher с nxio stat/realpath),
  `NativeThread` без сигналов (current = −1), `preClose0` = `shutdown`
  (освобождает поток, заблокированный в recv), `FileChannel.map`
  READ_ONLY/PRIVATE эмулируется копией в памяти, READ_WRITE отклоняется,
  `transferTo` → Java-цикл, селектор через `nxio_socket_poll`.
* libnet: IPv4-only сборка (`AF_INET6` снят, `DONT_ENABLE_IPV6`), обёртки
  `NET_*` без сигнального прерывания (`horizon_net_close.c`, close = shutdown
  + close), `sin_len`, нет маркер-дескриптора deferred close, список
  интерфейсов через `SIOCGIFCONF` (как в 17/21), isReachable — TCP echo,
  DefaultProxySelector без системных прокси.
* libmanagement: `OperatingSystemImpl` на лимитах процесса Horizon (из 17).

**Платформенные классы (`lib/horizon.jar`, исходники `jdk/src/horizon/classes`).**
В JDK 8 выбор реализаций идёт по `os.name` и для неизвестной ОС падает
(`AssertionError/InternalError/Error`). Добавлены: `DefaultFileSystemProvider`
(+ `HorizonFileSystemProvider/FileSystem/FileStore`: только basic-атрибуты,
`PollingWatchService`), `SocketDispatcher` (сокеты не через файловый мост),
`FileKey`, `NativeSeedGenerator` (CSRNG вместо `/dev/random`), `ProcessImpl`
(чистый `IOException` вместо падения `UNIXProcess`),
`DefaultAsynchronousChannelProvider` (явный `UnsupportedOperationException`),
`PortConfig`. `DefaultSelectorProvider` 8 сам выбирает poll-селектор.

## Ограничения и риски

* На устройстве **ничего не проверено**. Общий слой Horizon одинаков с 21
  (проверен на устройстве), но HotSpot 8 — другой код (деоптимизация,
  стеки, `ThreadLocalStorage`, CodeCache без сегментов), поэтому первые запуски
  — строго по плану ниже, начиная с `-Xint`.
* Нет AWT/Java2D/шрифтов/звука: любой код, создающий `BufferedImage`,
  `ImageIO`, `Font`, `javax.sound`, получит `UnsatisfiedLinkError`. Для MC
  ≤ 1.12.2 это блокер (текстуры грузятся через `ImageIO/BufferedImage`).
  `java.awt.graphicsenv` указывает на ещё не существующий класс.
* Только IPv4; нет процессов, асинхронных каналов, inotify, POSIX-атрибутов,
  `getFileStores()` пуст (но `Files.getFileStore(path)` работает), mmap только
  как копия, нет JVMTI/агентов, CDS, JFR, attach.
* `-XX:-UseCompressedOops` в пробнике унаследован от 21/17 (CodeMemory/nxvm
  раскладка); влияние на кучу MC 1.12 надо измерить.
* `runtime/memory`, `runtime/io`, `runtime/safefetch` — общие исходники с 21 и
  17; их правки затрагивают все три порта.

## План проверки на устройстве (выполняет владелец)

Каждый запуск ставит/проверяет `sdmc:/switch/mineswitch/runtime-8/` по
`files.manifest` (первый — ~80 MB копирования). Java 21/17 runtime не
затрагиваются. После каждого запуска Sphaira нужно открыть снова.

1. Базовая JVM: `bash scripts/run-nro.sh IP runtime/java8/launcher/mineswitch-java8-probe.nro`
   — ожидаются `JNI_CREATE_END result=0`, `java.version=1.8.0_492`,
   `os.name=Horizon`, `MINESWITCH_JAVA_PASS checks=46`, `JNI_DESTROY_END result=0`,
   `MINESWITCH_JVM_END result=0`.
2. Если пункт 1 падает — тот же NRO с `--jvm-options=romfs:/launch/interpreter.options`
   (`-Xint`): отделяет ошибки слоя ОС/GC от JIT.
3. C1: `… mineswitch-java8-probe.nro --jvm-options=romfs:/launch/c1-eager.options`
   — тот же PASS и строки `PrintCompilation` уровней 1–3.
4. C2: `… --jvm-options=romfs:/launch/c2-eager.options` — PASS и строки C2.
5. Графика: `bash scripts/run-nro.sh IP runtime/java8/launcher/mineswitch-java8-lwjgl.nro --graphics --system-main --class=mineswitch.probe.LwjglProbe --jvm-options=romfs:/launch/lwjgl-probe.options`
   — `MINESWITCH_LWJGL_PASS … frames=90`.
6. После интеграции: ванильный 1.16.5 и 1.14.4 на Java 8 (и сравнение с тем же
   на Java 17), затем Forge 1.12.2 — только после AWT и LWJGL 2.

При сбое прислать полный NXLink-лог и `sdmc:/switch/mineswitch/hs_err_pid*.log`.

## Нативные компоненты по диапазонам Minecraft

| Версии MC | Java | LWJGL | Что нужно сверх готового |
| --- | --- | --- | --- |
| 1.13 – 1.16.5 | 8 (официально); ванильные обычно работают и на 17 | 3.2.1/3.2.2 | Horizon LWJGL 3.3.3 (jar `--release 8`, нативы общие с 21/17) — уже слинкован в `mineswitch-java8-lwjgl.nro`; подмена 3.2.x → 3.3.x — штатная практика ARM-Mac/PojavLauncher, но проверять на каждой версии. Проверить, нужен ли headless AWT (у 17/21 `java.desktop` в профиле есть). |
| 1.7.10 – 1.12.2 | 8 | 2.9.x (+ jinput) | (а) headless libawt/Java2D (+ libjpeg, lcms, mlib_image, fontmanager при необходимости) — по `jdk/make/lib/Awt2dLibraries.gmk`, с Horizon `awt_LoadLibrary` как в 17/21; (б) **LWJGL 2**: рекомендуется не портировать нативы LWJGL 2 (их Display/Input завязаны на X11/GLX), а использовать слой совместимости LWJGL2-API поверх LWJGL 3 (подход lwjglx/PojavLauncher) на уже работающем Horizon LWJGL 3.3.3 + GLFW/EGL; jinput — заглушка, ввод через GLFW. |

Отдельно: Java 8 нужен прежде всего Forge 1.7.10–1.12.2 (LaunchWrapper
кастует системный загрузчик к `URLClassLoader`) и Forge до 1.16.5; ванильные
1.13–1.16.5 разумно сначала пробовать на уже собранном Java 17.

## Раскладка runtime-8 на SD

```
sdmc:/switch/mineswitch/runtime-8/
  release                  JAVA_VERSION="1.8.0_492", OS_NAME="Horizon"
  files.manifest           sha256 размер путь — проверяется установщиком
  lib/horizon.jar          платформенные классы Horizon (первые в boot path)
  lib/rt.jar jsse.jar jce.jar charsets.jar resources.jar
  lib/ext/localedata.jar cldrdata.jar sunjce_provider.jar zipfs.jar dnsns.jar
  lib/security/…  lib/cmm/…  lib/tzdb.dat currency.data *.properties
  mineswitch/libraries/    (только графический вариант) LWJGL-jar и lwjgl-probe.jar
```

Нативного кода в `runtime-8` нет: VM и все JNI-библиотеки статически в NRO.
`lib/aarch64` (dll_dir) существует только как имя для `sun.boot.library.path`.

## Контракт интеграции с лаунчером (не реализован, только описание)

Тот же, что для Java 17 (`docs/JAVA17_PORT.md`), с отличиями Java 8:

* **Отдельный NRO** `sdmc:/switch/mineswitch/mineswitch-java8.nro` (JVM 8 +
  LWJGL + код запуска игры). Причины те же: две статические HotSpot в одном
  ELF несовместимы (символы `JNI_*`/`JVM_*`/`Java_*`, один обработчик
  исключений libnx, один реестр, один malloc-оверлей), а резидентный код
  второй JVM отнимает память у кучи.
* **Выбор:** `javaVersion.majorVersion` из JSON версии Mojang (у старых версий
  — `jre-legacy`, 8) → Java 8; переопределение в метаданных инстанса
  (`javaMajor`), например, «1.16.5 ваниль на 17».
* **Переход:** фронтенд пишет `sdmc:/switch/mineswitch/launch/next.json`
  (id инстанса, файл опций, каталог, main class, аргументы), вызывает
  `envSetNextLoad("sdmc:/switch/mineswitch/mineswitch-java8.nro", "mineswitch-java8.nro --instance=<id>")`
  и обычным возвратом из `main` до создания JVM уходит в hbloader (не
  `horizon_exit_process`).
* **Runtime:** `java.home=/switch/mineswitch/runtime-8` (зашит и в
  `os_horizon.cpp`, переопределяется `MINESWITCH_JAVA_HOME`). Это JRE-раскладка
  без `lib/modules`/jimage. Установку можно вынести из NRO в общий установщик.
* **Опции JVM (отдельный файл на Java 8):** без `--add-modules`, `-Xlog`,
  `-Djdk.image.*`, `SweeperThreshold`; обязательны `-Xrs`, `-XX:+UseSerialGC`,
  `-XX:-UsePerfData`, `-XX:ReservedCodeCacheSize=64m…128m`; GC-лог —
  `-XX:+PrintGC`. Неизвестный флаг роняет `JNI_CreateJavaVM`.
* **Classpath:** разделитель `;` (как в 17/21); `-Djava.class.path`,
  `-Xbootclasspath/a:` работают; `-Xbootclasspath/p:` не трогать — первым
  уже стоит `lib/horizon.jar`.
* **Java-помощники** лаунчера (`ClientEntry`, `mineswitch-compat.jar`, …)
  собирать для 8 (`--release 8` у JDK ≥ 9 или `-source/-target 8
  -bootclasspath rt.jar`), без API 9+ (`List.of`, `var`, модули).
* **Сборка NRO:** образец — `runtime/java8/launcher/Makefile` (`GRAPHICS=1`) и
  `runtime/java8/generated-lwjgl/native-archives.mk`; заголовки JNI —
  `jdk/src/share/javavm/export` и `jdk/src/solaris/javavm/export`.

## Что осталось

1. Тесты на устройстве по плану (пункты 1–5).
2. Headless AWT/Java2D для 8 (libawt, libawt_headless, libjpeg, liblcms,
   libmlib_image; Horizon `awt_LoadLibrary`, класс `java.awt.graphicsenv`).
3. Слой LWJGL2-API поверх Horizon LWJGL 3.3.3 для 1.7.10–1.12.2.
4. Проверить замыкание классов MC 1.12.2/1.16.5 (аналог
   `runtime/analyze-minecraft-modules.py` для rt.jar) и опции памяти.
5. Интеграция в лаунчер по контракту выше.

## Следующий шаг

Запустить пункты 1–5 плана на устройстве. При успешном пробнике — начать
headless AWT для 8 и параллельно проверить ванильный 1.16.5 на Java 8 и 17.
