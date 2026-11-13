"""Exercise Application::EvaluateListeningGuard from real C++ sources on host."""

import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


def extract_block(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ListeningWatchdogTests(unittest.TestCase):
    def test_evaluate_listening_guard_state_transitions(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++23 compiler")

        app_h = (ROOT / "main/application.h").read_text(encoding="utf-8")
        app_cc = (ROOT / "main/application.cc").read_text(encoding="utf-8")

        enum_block = extract_block(app_h, "enum class ListeningGuardAction") + ";"
        const_lines = "\n".join(
            line.strip()
            for line in app_h.splitlines()
            if "kListening" in line and "constexpr int" in line
        )
        eval_fn = extract_block(
            app_cc,
            "Application::ListeningGuardAction Application::EvaluateListeningGuard",
        )

        driver = textwrap.dedent(
            f"""
            #include <cassert>

            enum ListeningMode {{
                kListeningModeAutoStop,
                kListeningModeManualStop,
                kListeningModeRealtime,
            }};

            class Application {{
            public:
                {enum_block}
                {const_lines}
                static ListeningGuardAction EvaluateListeningGuard(
                    ListeningMode mode,
                    bool pending_start,
                    bool voice_detected,
                    int elapsed_ticks,
                    int silence_ticks,
                    bool speech_seen);
            }};

            {eval_fn}

            int main() {{
                using Action = Application::ListeningGuardAction;

                // Manual / realtime / pending_start never auto-stop
                assert(Application::EvaluateListeningGuard(
                    kListeningModeManualStop, false, false, 99, 99, true) == Action::kContinue);
                assert(Application::EvaluateListeningGuard(
                    kListeningModeRealtime, false, false, 99, 99, false) == Action::kContinue);
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, true, false, 20, 20, false) == Action::kContinue);

                // No-speech timeout fires at kListeningNoSpeechTimeoutTicks (10s) when silent
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, false, 9, 9, false) == Action::kContinue);
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, false, 10, 10, false) == Action::kStopNoSpeechTimeout);

                // Speech followed by trailing silence stops at kListeningPostSpeechSilenceTicks (4s)
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, true, 12, 0, true) == Action::kContinue);
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, false, 15, 3, true) == Action::kContinue);
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, false, 16, 4, true) == Action::kStopSilenceAfterSpeech);
                // Active speech on current tick keeps listening even if silence_ticks hadn't cleared yet
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, true, 16, 4, true) == Action::kContinue);

                // Hard ceiling prevents wedged Listening if AFE VAD remains true continuously
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, true, 64, 0, true) == Action::kContinue);
                assert(Application::EvaluateListeningGuard(
                    kListeningModeAutoStop, false, true, 65, 0, true) == Action::kStopMaxDuration);
                return 0;
            }}
            """
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            src = pathlib.Path(tmpdir) / "watchdog_test.cc"
            exe = pathlib.Path(tmpdir) / "watchdog_test"
            src.write_text(driver, encoding="utf-8")
            compiled = subprocess.run(
                compiler + ["-std=c++23", str(src), "-o", str(exe)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_idle_alert_also_exits_listening_state(self):
        app_cc = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn('strcmp(status->valuestring, "待机") == 0', app_cc)
        self.assertIn("EvaluateListeningGuard(", app_cc)


if __name__ == "__main__":
    unittest.main()
