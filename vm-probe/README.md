# Проверка зависимостей JVM на Switch

`mineswitch-vm-probe.nro` проверяет **C-библиотеки, необходимые будущему порту**
OpenJDK Zero. Программа не содержит JVM, не исполняет Java-байткод и не проверяет
JNI или Minecraft.

Проверки:

- `libffi 3.4.8`: реальные `ffi_call` с 64-битным integer, signed 32-bit integer
  и указателем; mixed double/integer; десять integer-аргументов, в том числе
  передаваемые через стек AArch64 ABI.
- pthread/libsysbase/libnx: запуск и join рабочего потока, передача результата,
  TLS отдельно у главного и рабочего потока, вызов деструктора TLS при выходе.
- Mutex/condition variable: передача данных под mutex, сигнал другому потоку,
  `pthread_cond_timedwait` с `CLOCK_MONOTONIC`, ожидаемый таймаут 200 ms и
  повторное владение mutex после ожидания.
- Запрос `pthread_attr_setguardsize` выводится как отдельная возможность.
  `ENOSYS` здесь ожидаемо для текущего SDK и не считается провалом остальных
  проверок; полноценные guard pages этим тестом не проверяются.

## Сборка

Из корня проекта:

```sh
bash vm-probe/build.sh -j2
```

Нужна установленная toolchain проекта (`python3 scripts/bootstrap-toolchain.py`),
Python 3.12+, Make. Можно указать собственные `DEVKITPRO`/`DEVKITA64`.

Bootstrap скачивает официальный архив
[libffi 3.4.8](https://github.com/libffi/libffi/releases/tag/v3.4.8), проверяет
закреплённый SHA256 и собирает статическую библиотеку без изменения исходников.
SDK и системные каталоги не изменяются. Исходники, логи сборки, staging и манифест
кеша находятся в `.research/vm-probe-libffi/`.

Повторная сборка проверяет конфигурацию и хеши артефактов, затем использует кеш.
Компиляция NRO включает `-Wall -Wextra -Werror -std=gnu11`.

## Запуск

Оставить Sphaira открытой, NXLink включённым. Из корня проекта:

```sh
bash scripts/run-nro.sh 192.168.3.177 vm-probe/mineswitch-vm-probe.nro
```

Адрес — текущий Switch стенда. Лог содержит маркеры:

```text
MINESWITCH_VM_PROBE_BEGIN
MINESWITCH_VM_PROBE_CHECKS_COMPLETE passed=... failed=...
MINESWITCH_VM_PROBE_END passed=... failed=... exit=loader
```

Выход — кнопкой `+` или автоматически через 10 секунд после проверок.
Следует дополнительно убедиться, что Sphaira снова доступна: отправка конечного
маркера предшествует окончательному закрытию приложения.

Сетевой лог использует отдельный `FILE *`, который закрывается до `socketExit`.
Рабочий поток не пишет в консоль/сеть. В коде теста нет бесконечных mutex/condition
waits: mutex берётся через ограниченный `trylock`, условия имеют deadline,
а перед `pthread_join` проверяется завершение native-потока с таймаутом 3 s.
Это ограничивает ожидания самого теста, но не является общим watchdog: внутри
SDK `pthread_create` ждёт запуска потока, а сетевое подключение/вывод используют
обычные блокирующие функции.
Если поток не завершился, его состояние не освобождается и приложение завершает
**процесс** обычным `svcExitProcess` вместо возврата в загрузчик с живым потоком.
В этой ошибочной ветке Switch может вернуться в HOME; лог помечается `exit=process`.

## Границы результата

На OLED/Horizon 21.1.0 в режиме приложения 6 октября 2026 выполнен запуск
версии 0.1.0: **41 PASS, 0 FAIL**. Все три libffi-вызова дали ожидаемые значения,
pthread/create/join, mutex/condition и TLS прошли. Измеренный condition timeout —
200.00 ms с `ETIMEDOUT` (116); guard attribute вернул `ENOSYS` (88).

Лог: `logs/mineswitch-vm-probe-20261005T233334Z.log`; получены все маркеры,
включая `END ... exit=loader`, и завершение NXLink. Размер проверенного NRO —
248005 байт, SHA256:
`a0f02d5c9b84e5e7b84e65ab5211bd7d775b8b1cbdaa93d2be3ccb276f4977c0`.

Прохождение проверок подтверждает только использованные API и сигнатуры на
конкретном устройстве/SDK. Не проверены libffi closure callbacks, исполняемые
trampolines, остальные pthread APIs, весь AArch64 ABI или многопоточная JVM.
TLS-тест проверяет один вызов одного деструктора; повторные проходы деструкторов
и исчерпание доступных TLS slots не проверяются.
Полный план и ограничения: [Java 21 для Horizon](../docs/JAVA21_PORT.md).
