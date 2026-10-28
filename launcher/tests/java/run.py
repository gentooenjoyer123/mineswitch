#!/usr/bin/env python3
"""Compile and run ordinary local Java 21 processor fixtures.

Run through the repository build limiter:
  python3 runtime/limit-build.py -- python3 launcher/tests/java/run.py
No official processor jars, mod binaries, network or console are used.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
TESTS = ROOT / "launcher/tests/java"
CHECKS = 0


def check(condition, label):
    global CHECKS
    if not condition:
        raise AssertionError(label)
    CHECKS += 1


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def write_plan(path, values):
    def escape(text):
        return str(text).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")
    path.write_text("\n".join(f"{key}={escape(value)}" for key, value in values.items()) + "\n", encoding="utf-8")


def completion(path):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            result[key] = value.replace("\\:", ":").replace("\\=", "=")
    return result


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        raise SystemExit("Run through python3 runtime/limit-build.py -- python3 launcher/tests/java/run.py")
    java, javac, jar = "java", "javac", "jar"
    version = subprocess.check_output([javac, "-version"], text=True)
    check(version.startswith("javac 21."), "Host fixture requires Java 21")
    with tempfile.TemporaryDirectory(prefix="mineswitch-processors-") as temporary:
        temp = Path(temporary)
        storage = temp / "library café"
        storage.mkdir()
        classes, processors, dependency = (temp / item for item in ("classes", "processors", "dependency"))
        for directory in (classes, processors, dependency):
            directory.mkdir()
        run([javac, "--release", "21", "-Xlint:all", "-Werror", "-d", str(classes),
             str(ROOT / "launcher/java/mineswitch/launcher/ProcessorEntry.java"), str(TESTS / "InvocationHarness.java")])
        run([javac, "--release", "21", "-d", str(dependency), str(TESTS / "dependency/Message.java")])
        run([javac, "--release", "21", "-cp", str(dependency), "-d", str(processors),
             str(TESTS / "fixtures/WriteProcessor.java"), str(TESTS / "fixtures/ExitProcessor.java"),
             str(TESTS / "fixtures/ThrowProcessor.java")])
        tools = storage / "tools"
        tools.mkdir()
        library, writer, exiting, throwing = (tools / name for name in ("dependency.jar", "writer.jar", "exit.jar", "throw.jar"))
        run([jar, "--create", "--file", str(library), "-C", str(dependency), "."])
        run([jar, "--create", "--file", str(writer), "--main-class", "fixture.WriteProcessor", "-C", str(processors), "fixture/WriteProcessor.class"])
        run([jar, "--create", "--file", str(exiting), "--main-class", "fixture.ExitProcessor", "-C", str(processors), "fixture/ExitProcessor.class"])
        run([jar, "--create", "--file", str(throwing), "--main-class", "fixture.ThrowProcessor", "-C", str(processors), "fixture/ThrowProcessor.class"])
        original = storage / "client.jar"
        original.write_bytes(b"ordinary original client fixture")
        original_hash = hashlib.sha256(original.read_bytes()).hexdigest()

        def plan(name, steps):
            directory = storage / name
            directory.mkdir()
            filename, marker = directory / "plan.properties", directory / "complete.properties"
            values = {"schema": 1, "storage_root": storage, "completion_file": marker,
                      "input.count": 1, "input.0.path": original, "processor.count": len(steps)}
            for index, step in enumerate(steps):
                prefix = f"processor.{index}."
                values[prefix + "jar"] = step.get("jar", writer)
                values[prefix + "classpath.count"] = 1
                values[prefix + "classpath.0"] = library
                arguments = [directory / step["output"], step["text"], marker] if "output" in step else []
                arguments = step.get("args", arguments)
                values[prefix + "args.count"] = len(arguments)
                for i, argument in enumerate(arguments):
                    values[prefix + f"args.{i}"] = argument
                values[prefix + "outputs.count"] = int("output" in step)
                if "output" in step:
                    values[prefix + "outputs.0.path"] = directory / step["output"]
                    if "sha1" in step:
                        values[prefix + "outputs.0.sha1"] = step["sha1"]
            write_plan(filename, values)
            return filename, marker, values

        def invoke(filename, expect=0, harness=False, failure=False):
            command = [java, "-cp", str(classes), "fixture.InvocationHarness" if harness else "mineswitch.launcher.ProcessorEntry", str(filename)]
            if failure:
                command.append("expect-failure")
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            check(result.returncode == expect if expect == 0 else result.returncode != 0,
                  f"Expected processor exit {expect}: {result.stdout}\n{result.stderr}")
            return result

        first_text = "fixture dependency: café"
        first_sha1 = hashlib.sha1(first_text.encode()).hexdigest()
        filename, marker, values = plan("success", [
            {"output": "work/first.txt", "text": "café", "sha1": first_sha1},
            {"output": "work/final.txt", "text": "@file:" + str(storage / "success/work/first.txt")},
        ])
        marker.write_text("old success", encoding="utf-8")
        Path(str(marker) + ".tmp").write_text("old temporary", encoding="utf-8")
        log = filename.parent / "processor.log"
        log.write_text("old processor output", encoding="utf-8")
        result = invoke(filename, harness=True)
        check("Processor context restored" in result.stdout, "Caller context classloader restored")
        check("Processor streams restored" in result.stdout, "Caller output streams restored")
        logged = log.read_text(encoding="utf-8")
        check("fixture stdout: café" in logged and "fixture stderr: café" in logged,
              "Tool stdout and stderr persisted with UTF-8")
        check("fixture stdout:" not in result.stdout and "fixture stderr:" not in result.stderr,
              "Verbose tool output does not reach the original console")
        check("Processor 1/2:" in result.stdout and "Processor 2/2 complete" in result.stdout,
              "Original console receives concise step start and end messages")
        check("old processor output" not in logged, "Each invocation replaces its previous log")
        status = completion(marker)
        check(status["schema"] == "1" and status["state"] == "complete", "Success schema")
        check(status["plan_file"] == str(filename) and status["plan_sha256"] == hashlib.sha256(filename.read_bytes()).hexdigest(),
              "Completion identifies exact raw plan")
        check(status["processor.count"] == "2" and status["output.count"] == "2", "All processors and outputs completed")
        check(status["output.0.sha1"] == first_sha1, "Declared SHA1 verified and recorded")
        for i in range(2):
            output = Path(status[f"output.{i}.path"])
            check(status[f"output.{i}.sha256"] == hashlib.sha256(output.read_bytes()).hexdigest(), "Every output has a verified SHA256")
        check(hashlib.sha256(original.read_bytes()).hexdigest() == original_hash, "Original client preserved")
        check(not Path(str(marker) + ".tmp").exists(), "Completion temporary consumed")

        # A normal processor with a mismatched expected result must not commit.
        filename, marker, _ = plan("checksum", [{"output": "result.txt", "text": "normal text", "sha1": "0" * 40}])
        result = invoke(filename, harness=True, failure=True)
        check(not marker.exists(), "Output checksum failure has no completion")
        check("Processor streams restored" in result.stdout and "Processor context restored" in result.stdout,
              "Caller state restored after output verification failure")
        check("Processor output SHA1 mismatch" in (filename.parent / "processor.log").read_text(encoding="utf-8"),
              "Output verification failure is recorded in the log")

        filename, marker, _ = plan("early-exit", [{"jar": exiting}])
        marker.write_text("stale success", encoding="utf-8")
        invoke(filename)
        check(not marker.exists(), "System.exit(0) from a processor cannot look complete")
        check("fixture early exit without newline" in (filename.parent / "processor.log").read_text(encoding="utf-8"),
              "System.exit flushes buffered tool output without a newline")

        filename, marker, _ = plan("closing-streams", [
            {"output": "first.txt", "text": "close-streams"},
            {"output": "second.txt", "text": "after-close"},
        ])
        result = invoke(filename, harness=True)
        check(marker.exists() and "Processor streams restored" in result.stdout, "Tool close cannot close caller streams")
        check("fixture stderr: after-close" in (filename.parent / "processor.log").read_text(encoding="utf-8"),
              "A later processor can still write the shared log")

        for name, arguments in (("throwing", []), ("flooding", ["flood"])):
            filename, marker, _ = plan(name, [{"jar": throwing, "args": arguments}])
            marker.write_text("stale success", encoding="utf-8")
            result = invoke(filename, harness=True, failure=True)
            logged = (filename.parent / "processor.log").read_bytes()
            check(not marker.exists(), "Throwing processor cannot publish completion")
            check("Processor context restored" in result.stdout and "Processor streams restored" in result.stdout,
                  "Caller state restored after processor changes streams/context and throws")
            check(b"java.lang.IllegalStateException: fixture failing processor" in logged and b"fixture.ThrowProcessor.main" in logged,
                  "Processor exception and stack trace retained in file")
            check(len(result.stdout) + len(result.stderr) < 2000, "Failure console output remains concise")
            if arguments:
                check(len(logged) <= 8 * 1024 * 1024 and b"Verbose output truncated" in logged,
                      "Log cap preserves room for failure diagnostics after excessive output")

        for collision in ("plan", "completion", "input", "jar", "classpath", "output", "case-output", "hardlink", "symlink"):
            filename, marker, values = plan("log-collision-" + collision, [{"output": "result.txt", "text": "normal text"}])
            log = filename.parent / "processor.log"
            previous = None
            if collision == "plan":
                log.write_bytes(filename.read_bytes())
                filename = log
                previous = log.read_bytes()
            elif collision == "completion":
                values["completion_file"] = log
            elif collision == "input":
                log.write_bytes(b"protected log input")
                previous = log.read_bytes()
                values["input.count"] = 2
                values["input.1.path"] = log
            elif collision in ("jar", "classpath"):
                log.write_bytes(writer.read_bytes() if collision == "jar" else library.read_bytes())
                previous = log.read_bytes()
                values["processor.0.jar" if collision == "jar" else "processor.0.classpath.0"] = log
            elif collision in ("output", "case-output"):
                target = log if collision == "output" else log.with_name("PROCESSOR.LOG")
                values["processor.0.outputs.0.path"] = target
                values["processor.0.args.0"] = target
            elif collision == "hardlink":
                os.link(original, log)
                previous = original.read_bytes()
            else:
                log.symlink_to(original)
                previous = original.read_bytes()
            if collision != "plan":
                write_plan(filename, values)
            invoke(filename, expect=1)
            check(not marker.exists() and hashlib.sha256(original.read_bytes()).hexdigest() == original_hash,
                  "Log path collision rejected without changing original input: " + collision)
            if previous is not None:
                check(log.read_bytes() == previous, "Rejected log path leaves prior file intact: " + collision)

        filename, marker, values = plan("protected-input", [{"output": "result.txt", "text": "normal text"}])
        values["processor.0.outputs.0.path"] = original
        values["processor.0.args.0"] = original
        write_plan(filename, values)
        invoke(filename, expect=1)
        check(not marker.exists() and hashlib.sha256(original.read_bytes()).hexdigest() == original_hash,
              "Declared output cannot overwrite protected client input")

        filename, marker, values = plan("count-limit", [])
        values["processor.count"] = 129
        write_plan(filename, values)
        invoke(filename, expect=1)
        check(not marker.exists(), "Processor count bound")

        filename, marker, values = plan("path-bound", [{"output": "result.txt", "text": "normal text"}])
        values["processor.0.outputs.0.path"] = temp / "outside.txt"
        write_plan(filename, values)
        invoke(filename, expect=1)
        check(not marker.exists() and not (temp / "outside.txt").exists(), "Declared paths must remain in storage")

        filename, marker, _ = plan("duplicate-field", [])
        with filename.open("a", encoding="utf-8") as stream:
            stream.write("schema=1\n")
        invoke(filename, expect=1)
        check(not marker.exists(), "Duplicate plan fields rejected")
        print(f"Processor tests passed: {CHECKS} checks")


if __name__ == "__main__":
    main()
