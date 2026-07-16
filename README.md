# suck - A Pipe Fitting That Kills on Silence

`suck` is a command-line utility designed to assist automated agents and local AI pipelines that get hung up on interactive or blocking `2>&1` streams, interrupting fruitless, silent looping.

If a subprocess hangs indefinitely without output, `suck` steps in, terminates the stalled pipe, and exits with a distinctive status code.

---

### Usage

```bash
suck [-t SEC] [-T SEC] [-q] [-n N] [-v] [command [args...]]

```

### Options

* **`-t SEC`** - Silence timeout. Terminate process if no output is received for `SEC` seconds. `[0=off]`
* **`-T SEC`** - Hard timeout. Force terminate after `SEC` seconds regardless of activity. `[0=off]`
* **`-q`** - Quiet mode. Buffer stdout and only print the tail on a timeout event.
* **`-n N`** - Ring buffer line limit for quiet (`-q`) mode. `[100]`
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

---

### Why it exists

Automated code-runners and LLM agents often get stuck executing commands that expect user input, prompt for configurations, or enter infinite loops. Standard Unix `timeout` utilities only handle absolute runtime limits. `suck` dynamically inspects the stdout/stderr pipe, acting as an active heartbeat monitor that pulls the plug the moment a task falls silent.

