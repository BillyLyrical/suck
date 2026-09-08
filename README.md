# suck 
monitor data streams and reap stagnant processes

`suck` is a command-line utility designed to assist automated agents and local AI pipelines that get hung up on interactive or blocking `2>&1` streams, interrupting fruitless, silent looping.

If a subprocess hangs indefinitely without output, `suck` steps in, terminates the stalled pipe, and exits with a distinctive status code.

---

### Usage

```bash
suck [-t SEC] [-T SEC] [-q] [-n N] [-E] [-e FILE] [-P] [-I] [-i REGEX] [-v] [command [args...]]

```

### Options

* **`-t SEC`** - Silence timeout. Terminate process if no output is received for `SEC` seconds. `[0=off]`
* **`-T SEC`** - Hard timeout. Force terminate after `SEC` seconds regardless of activity. `[0=off]`
* **`-q`** - Quiet mode. Buffer stdout and only print the tail on a timeout event.
* **`-n N`** - Ring buffer line limit for quiet (`-q`) mode. `[100]`
* **`-E`** - Discard command's STDERR (redirect to `/dev/null`).
* **`-e FILE`** - Redirect command's STDERR to a dedicated log file.
* **`-P`** - Fast-fail panic. Kill immediately if any data hits STDERR.
* **`-I`** - Non-interactive mode. Redirect STDIN from `/dev/null`.
* **`-i REGEX`** - Ignore pattern. Lines matching REGEX do not reset silence timer.
* **`-v`** - Verbose status messages printed directly to stderr.


### Exit Codes

* **`0`** - Normal completion
* **`124`** - Hard timeout reached (`-T`)
* **`125`** - Silence timeout reached (`-t`)
* **`127`** - Command execution failed

---

### Examples

**Monitor a long build and kill it if it hangs for a minute:**

```bash
perlbrew install-cpanm | suck -t 60

```

**Run a compile with a 1-hour hard limit and 30-second silence watchdog:**

```bash
suck -t 30 -T 3600 make -j4

```

**Run a noisy build quietly, dumping only the last 50 lines of output if it stalls:**

```bash
suck -t 60 -q -n 50 ./slow-build.sh

```

**See debug output on stderr while waiting for a module installer:**

```bash
cpanm DBI | suck -t 10 -v

```

**Run an isolated application sandbox while completely suppressing noisy container layout warnings from standard error:**

```bash
suck -t 10 -E bwrap --ro-bind /sys /sys --dev /dev ./facaded

```

**Launch an MCP daemon, routing the protocol stream cleanly through standard output while capturing internal diagnostics to a dedicated log file:**

```bash
suck -t 60 -e ./logs/faced_debug.log ./faced --mode daemon

```

**Monitor a model weight layer calculation, aborting instantly the moment an execution error or allocation crash prints to standard error:**

```bash
suck -t 45 -P ./ai-harness --compute-weights

```

**Execute an automated compilation script, ensuring it fails immediately instead of hanging indefinitely if an unhandled verification or interactive setup prompt appears:**

```bash
suck -t 15 -I ./setup_env.sh

```

**Track a background network build process, ignoring routine heartbeat log lines so they do not falsely reset the silence watchdog:**

```bash
suck -t 20 -i "Retrying connection" ./network-sync.sh

```

**Run a long-running testing suite silently, only dumping the final 50 lines of output if a 10-second silence threshold is breached:**

```bash
suck -t 10 -q -n 50 ./test_runner

```


---

### Why it exists

Automated code-runners and LLM agents often get stuck executing commands that expect user input, prompt for configurations, or enter infinite loops. Standard Unix `timeout` utilities only handle absolute runtime limits. `suck` dynamically inspects the stdout/stderr pipe, acting as an active heartbeat monitor that pulls the plug the moment a task falls silent.

