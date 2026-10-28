# Java 21 для Horizon: выбранный путь и проверенные ограничения

Дата исследования: 6 октября 2026. Цель — Minecraft Java 1.21.1.
Последующее состояние: OpenJDK 21 Zero прошёл 21 Java-проверку, а нативный
AArch64 HotSpot C1/C2 — по 45 JIT-проверок и по 31 проверке runtime-контрактов
на Switch. Native LWJGL прошёл 24 проверки и 90 кадров. Актуальные логи и пределы
этих результатов: [JIT_BRINGUP.md](JIT_BRINGUP.md),
[PORT_PROGRESS.md](PORT_PROGRESS.md).
Ниже сохранены исходное обоснование и отдельные ранние тесты C. Они сами по
себе не являются доказательством запуска JVM; такое доказательство — отдельный
лог `logs/mineswitch-jvm-20261006T004427Z.log` и последующие native device-логи.

## Решение

Первую настоящую JVM строить из **OpenJDK 21u, HotSpot Zero**, с новым слоем ОС
Horizon и стандартной библиотекой той же версии. Zero — существующий C++
интерпретатор HotSpot, сохраняющий инфраструктуру загрузки классов, верификации,
GC и JNI. Это существенно меньше работы, чем собственная JVM или расширение
экспериментальной Java 8 VM до Java 21.

Zero служит этапом проверки корректности порта. Играбельность Minecraft с ним
не предполагается и не измерена. После работающего ядра Java следующий этап —
порт обычного AArch64 HotSpot: сначала машинный интерпретатор, затем C1/C2.
Общий слой ОС, Java-библиотеки и большая часть native-кода должны использоваться
обоими вариантами.

Даже `-Xint` обычного AArch64 HotSpot не устраняет потребность в исполняемой
динамической памяти: он генерирует машинный интерпретатор и runtime stubs.
Zero позволяет отложить именно эту часть. В Zero вызов native-метода проходит
через `ffi_call` из libffi; байткод по-прежнему выполняется интерпретатором.

Источники OpenJDK для аудита закреплены на
[`bb6b279495ceff8442dbd0225756b92d466d5309`](https://github.com/openjdk/jdk21u/tree/bb6b279495ceff8442dbd0225756b92d466d5309).
Выбранные файлы и `SOURCE.json` сохранены в `.research/openjdk21/`. Это
исследовательский снимок исходников, а не готовая сборка или заявленная
совместимость со спецификацией Java SE.

## Что уже проверено на компьютере

Использован имеющийся devkitA64 GCC 15.2, libnx 4.12.0, AArch64/newlib;
результаты compile/link probes лежат в `.research/openjdk21-probes/audit.json`.

| Проверка | Результат и предел вывода |
| --- | --- |
| `<pthread.h>` + `pthread_create` | Компиляция и линковка прошли. Реализация приходит из **libsysbase.a**, автоматически добавляемой toolchain. Пустая `libpthread.a` не означает отсутствия pthread. |
| C11 `thrd_create` / `thrd_join` | Компиляция и линковка прошли. |
| `<sys/mman.h>` | Заголовок отсутствует в установленном target SDK; compile probe падает. Готового Unix memory API нет. |
| `<dlfcn.h>` | Заголовок отсутствует; compile probe падает. Linux `dlopen` нельзя считать доступным. |
| libffi 3.4.8 | Официальные исходники собраны без изменения в статическую AArch64-библиотеку. |
| `ffi_prep_cif` / `ffi_call` | Минимальный вызов с двумя `int64_t` успешно слинкован в target ELF. Последующая отдельная проверка на устройстве описана ниже. |

`libffi` скачана из
[официального выпуска 3.4.8](https://github.com/libffi/libffi/releases/tag/v3.4.8).
SHA256 архива:
`bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b`.
Исходники, логи configure/build и библиотека находятся в `.research/libffi/`.
Для воспроизводимой сборки NRO добавлен `vm-probe/bootstrap-libffi.py`; его
отдельный staging — `.research/vm-probe-libffi/stage/`.
Сборка использует `-march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC
-ftls-model=local-exec`, статическую линковку и `--disable-exec-static-tramp`.
Проверялся путь `ffi_call`; создание исполняемых closure-trampolines этим
результатом **не подтверждено**. Для callbacks LWJGL их потребуется разобрать
отдельно или использовать статические адаптеры.

## Проверено на реальном Switch

`vm-probe` 0.1.0 запущен на OLED/Horizon 21.1.0 в режиме приложения:
**41 проверка прошла, 0 ошибок**. Лог:
`logs/mineswitch-vm-probe-20261005T233334Z.log` (6 октября, 02:33 MSK).
Размер NRO — 248005 байт, SHA256:
`a0f02d5c9b84e5e7b84e65ab5211bd7d775b8b1cbdaa93d2be3ccb276f4977c0`.

| Проверка на устройстве | Результат |
| --- | --- |
| libffi: `int64_t`, signed `int32_t`, pointer | Результат 12884901930 совпал с ожидаемым. |
| libffi: double и integer в одном вызове | Результат −2.75 совпал. |
| libffi: десять integer-аргументов | Передача через регистры и стек дала ожидаемый результат 385. |
| pthread | Создание, передача результата, ограниченное ожидание native exit и join прошли. |
| Mutex / condition | Передача данных под mutex и пробуждение по сигналу прошли. |
| Timed condition wait | 200.00 ms при запросе 200 ms, `ETIMEDOUT` (116); mutex после ожидания разблокирован успешно. |
| TLS | Изоляция двух потоков, round-trip и один деструктор при выходе worker прошли. |
| Guard page attribute | `pthread_attr_setguardsize`: `ENOSYS` (88), ограничение подтверждено исполнением. |
| Завершение | Получены все маркеры лога, включая `END ... exit=loader`, и завершение NXLink. |

Это проверяет используемые C API/сигнатуры, **не JVM, JNI, полный pthread или
Java 21**. Не проверены libffi closures, повторная установка TLS из деструктора,
исчерпание TLS slots, detach/cancel и прочие сложные случаи. Таймауты в NRO
ограничивают наши ожидания worker; они не являются общим watchdog для внутренних
блокирующих вызовов SDK и сетевого вывода.

Отдельная диагностика в режиме приложения измерила **3189 MiB** бюджета процесса.
OpenGL-проверка получила OpenGL 4.3 Core, GLSL 4.30, nouveau NV120 / Mesa
20.1.0-rc3; шейдеры GLSL 150, отрисовка и чтение контрольных пикселей прошли.
В этой небольшой графической сцене allocator показывал около **108 MiB**
занятых данных. Это не прогноз потребления Minecraft и не независимая прибавка
к heap libnx. Лог графики: `logs/mineswitch-gl-20261005T233206Z.log`;
общий отчёт — [DEVICE_BASELINE.md](DEVICE_BASELINE.md).

## Почему недостаточно собрать Linux JDK для ARM64

1. `aarch64-none-elf` — toolchain с newlib для homebrew, а не Linux/glibc sysroot.
   В upstream configure нет Horizon. `make/autoconf/platform.m4` отвергает
   неизвестную ОС; добавить название ОС недостаточно.
2. `os_linux.cpp` и `os_posix.cpp` опираются на `mmap/mprotect/munmap`, POSIX
   signals, `pthread`, `dlopen/dlsym`, `/proc` и особенности Linux TLS/стеков.
   Нужно реализовать контракты HotSpot для Horizon, а не вернуть фиктивный успех
   из отсутствующих функций.
3. Помимо `libjvm` необходим native-код `java.base`: файловая система, ZIP,
   `libjimage`, часы, свойства ОС, сеть/NIO, charset, native-библиотеки.
   `rt.jar` от Java 8 не подходит к Java 21 с модульным runtime image.
4. Linux `.so` для ARM64 не загружаются как готовые homebrew-модули: совпадение
   набора инструкций не обеспечивает совместимость libc, загрузчика и API ОС.

Основные точки порта:

| Область | Реализация для первого этапа |
| --- | --- |
| Build system | Target `horizon`, раздельные build/target JDK tools, devkitA64 flags и NRO launcher; новые `hotspot/os/horizon` и `hotspot/os_cpu/horizon_zero`. Не выдавать target за Linux. |
| Потоки и TLS | Использовать проверенную часть pthread/libsysbase или libnx `Thread`, `Mutex`, `CondVar`, TLS. Сопоставить stack bounds, приоритеты, detach/join, таймауты и деструкторы TLS. |
| Виртуальная память | Реальные reserve/commit/uncommit/release/protect с учётом лимита процесса, выравнивания и guard pages. `virtmemAddReservation` резервирует адресный диапазон, но сама не выделяет физическую память. |
| GC / safepoints | Сначала Serial GC и ограниченный heap. Правильные остановка/возобновление Java-потоков, публикация roots и переходы native↔Java; не заменять отсутствующие механизмы пустыми функциями. |
| Exceptions / faults | Явные Java-проверки Zero помогают, но обработку native faults и переполнения стека всё равно необходимо определить. POSIX signal handler из Linux напрямую не подходит. |
| Статические natives | Один NRO с libjvm/libjava/libzip/libjimage и нужными библиотеками; таблица поиска символов для встроенных библиотек. JNI `RegisterNatives`/built-in library API предпочтительнее собственного динамического ELF-загрузчика на первом этапе. |
| Runtime image | Получать классы и native-часть из одного pinned дерева JDK. Начать с `java.base`, явно определить требуемые модули игры позднее. |
| Файлы и сеть | Имена `sdmc:/...`/`romfs:/...`, обычное чтение и ZIP сначала. NIO, selectors, `MappedByteBuffer`, DNS/TLS и сохранения проверять отдельно. |

Память libnx по умолчанию может занимать почти весь разрешённый heap процесса.
Её нельзя автоматически считать отдельным резервом сверх Java heap: управление
backing memory JVM нужно согласовать с libnx/newlib. На старте следует проверить
небольшой Java heap (например 64 MiB); это не предложенный бюджет Minecraft.

## pthread есть, но Linux-совместимость неполная

Первичный источник —
[`devkitPro/newlib`, `libgloss/libsysbase/pthread.c`](https://github.com/devkitPro/newlib/blob/57bd8e1ade3cf46d2e61a72516ac8bfaf8fdfd62/libgloss/libsysbase/pthread.c).
Здесь `pthread_attr_getguardsize`, `pthread_attr_setguardsize`, cancellation
и часть других функций возвращают `ENOSYS`. Наличие экспортированного символа
не подтверждает его функциональность. В установленном `libsysbase.a` возврат
`ENOSYS` у `pthread_attr_setguardsize` также подтверждён дизассемблированием.

Linux Zero читает границы стека через `pthread_getattr_np` и сведения о guard
pages; этот код надо заменить учётом стеков libnx. Использовать базовые pthread
операции возможно: их ограниченный runtime probe уже прошёл, как описано выше.
Имеющийся более ранний тест `Thread` из libnx сам по себе не проверял слой
pthread/libsysbase. Для JVM остаётся проверить и реализовать остальные
необходимые контракты.

Дополнительно в исследованном `pthread.c` `pthread_getcpuclockid` возвращает
`CLOCK_REALTIME`, а реализация `pthread_create` не использует настройку detach
из атрибутов. В [libnx newlib adapter](https://github.com/switchbrew/libnx/blob/feebd026ca0f5dcc2119f46ad8e0d16ad3dd4973/nx/source/runtime/newlib.c)
реализация `__syscall_thread_detach` отсутствует. Это конкретные расхождения,
которые нужно учесть при переносе Linux-кода, даже если обычный create/join
успешен.

## Статическая сборка — существующая основа, а не готовый флаг для Switch

В OpenJDK 21 есть `--enable-static-build` и HotSpot feature `static-build`, но
[`jdk-options.m4`](https://github.com/openjdk/jdk21u/blob/bb6b279495ceff8442dbd0225756b92d466d5309/make/autoconf/jdk-options.m4)
разрешает этот режим только для macOS. Нужно расширить build integration для
Horizon и проверить состав/порядок архивов, удержание native-symbols линкером,
инициализацию и завершение.

[`NativeLibraries.c`](https://github.com/openjdk/jdk21u/blob/bb6b279495ceff8442dbd0225756b92d466d5309/src/java.base/share/native/libjava/NativeLibraries.c)
уже поддерживает встроенные JNI-библиотеки с `JNI_OnLoad_<имя>`. Однако поиск
реализован через `JVM_FindLibraryEntry`; для Horizon требуется рабочий resolver
встроенных символов. Просто добавить `-static` или заменить `dlopen` на успешную
заглушку недостаточно.

LWJGL дополнительно ищет native-функции по адресам и использует callbacks. Даже
после запуска JVM потребуется адаптировать его loader, bindings GLFW/OpenAL и
получение адресов OpenGL. Эти задачи не решаются только запуском `HelloWorld`.

## Почему не брать имеющиеся Java-проекты Switch

- [javm-nx-poc](https://github.com/XorTroll/javm-nx-poc) архивирован; его README
  прямо говорит, что разработка прекращена.
- [libjavm](https://github.com/XorTroll/libjavm) использует `rt.jar`, заявляет
  экспериментальную Java 8 или ниже; `invokedynamic` и стандартные native-методы
  не полностью реализованы. Это не основа готовой Java 21.
- [SwitchGDX](https://github.com/SwitchGDX/switch-gdx) действительно запускает
  портированные LibGDX-игры на Switch, но использует
  [Clearwing](https://github.com/TheLogicMaster/clearwing-vm): Java→C++ AOT и
  ограниченный runtime Java 7/8. Нет полноценного JNI library loading и
  совместимости Java 21. Его libffi/build опыт полезен, но это другая модель порта.

Проверены указанные проекты и upstream JDK; отсутствие найденного готового порта
не является доказательством отсутствия любых частных разработок.

## Первый сборочный milestone и критерии результата

**Завершённый подготовительный этап:** отдельный `vm-probe` NRO проверил
собранную libffi и базовые pthread API на реальном Switch. Он проверяет только
зависимости среды C; Java и JNI ещё не запущены.

**Следующая работа:** полноценное дерево OpenJDK 21u с Horizon target,
раздельными build/target tools и реализацией OS-слоя. Сначала добиться
компиляции реальных объектов HotSpot Zero и явного списка оставшихся platform
symbols, затем линковки статического `libjvm`. Здесь пока нет готовой реализации.

**Первый milestone самой JVM:** Horizon target собирает статический Zero
`libjvm` и минимальный NRO с реальным `JNI_CreateJavaVM`; тот загружает runtime
image Java 21, исполняет обычный class-файл версии 65 и корректно завершает VM.
Нужны вывод `java.version`, загрузка класса из JAR/SD, арифметика, исключение,
`String`, `Thread.start/join`, небольшая аллокационная нагрузка с реальным GC
и вызов одного зарегистрированного native-метода с обратной передачей результата.
До исполнения этих проверок результат нельзя называть работающей JVM 21.

Для этого milestone отключить необязательные JFR/JVMTI/attach/CDS и сложные GC
в выбранной конфигурации, не подменяя функциональность классов успешными
заглушками. Отсутствующие возможности явно отклонять. Наличие Java 21 bytecode
support у Zero не означает, что этот первый порт поддерживает все Java SE APIs,
виртуальные потоки, все библиотеки Minecraft или моды.

После него: воспроизводимые проверки Java-библиотек, стандартный AArch64
HotSpot и code cache через libnx JIT, затем LWJGL/звук/управление и меню игры.
Для HotSpot существенны раздельные writable/executable aliases libnx, адреса
инструкций, runtime patching и обслуживание instruction cache. Успешное
`jitCreate` не доказывает готовность такого code cache.

Оценка срока или FPS сейчас не обоснована: самый большой ещё не реализованный
блок — слой ОС OpenJDK и его контракты памяти/потоков, а не упаковка JAR в NRO.

## Дополнительные первичные ссылки

- [Zero variant / допустимые VM features](https://github.com/openjdk/jdk21u/blob/bb6b279495ceff8442dbd0225756b92d466d5309/make/autoconf/jvm-features.m4).
- [Zero native call через libffi](https://github.com/openjdk/jdk21u/blob/bb6b279495ceff8442dbd0225756b92d466d5309/src/hotspot/cpu/zero/zeroInterpreter_zero.cpp).
- [Linux Zero OS/CPU слой](https://github.com/openjdk/jdk21u/blob/bb6b279495ceff8442dbd0225756b92d466d5309/src/hotspot/os_cpu/linux_zero/os_linux_zero.cpp).
- [POSIX память, синхронизация и загрузка библиотек](https://github.com/openjdk/jdk21u/blob/bb6b279495ceff8442dbd0225756b92d466d5309/src/hotspot/os/posix/os_posix.cpp).
- [libnx Thread/TLS](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/thread.h),
  [virtual address reservations](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/virtmem.h),
  [JIT mappings](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/jit.h).
