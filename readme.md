# novaShell

![C Language](https://img.shields.io/badge/Written_in-C-blue?logo=c)
![Windows](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)
![Version](https://img.shields.io/badge/Version-2.0-green)  
![License](https://img.shields.io/badge/License-MIT-yellow)

novaShell is a small command shell for Windows, written in C as a single source file. It only uses the Win32 API and the C runtime, so there are no dependencies and nothing to install. The goal is to bring a few Linux-style conveniences to Windows (history search, bookmarks, background jobs, grep and friends, saved SSH connections) while still being able to run everything that works in `cmd.exe`.

Anything that is not a built-in command is handed to `cmd.exe`, so batch files, pipes, redirects and normal programs behave as usual.

## Table of contents

- [Building](#building)
- [Starting out](#starting-out)
- [The prompt](#the-prompt)
- [Line editing](#line-editing)
- [Tab completion](#tab-completion)
- [Command reference](#command-reference)
- [Chaining, pipes and background jobs](#chaining-pipes-and-background-jobs)
- [Exit status and notifications](#exit-status-and-notifications)
- [SSH profiles](#ssh-profiles)
- [Configuration](#configuration)
- [Limits and known quirks](#limits-and-known-quirks)

## Building

The whole shell lives in `main.c`.

With MSVC (from a developer command prompt):

```
cl /O2 main.c
```

With MinGW-w64:

```
gcc -O2 main.c -o novashell.exe
```

A 64-bit build is recommended. The OpenSSH client that ships with Windows lives in `System32\OpenSSH`, and a 32-bit process on 64-bit Windows gets redirected away from that folder, so `sshm` would not find `ssh.exe`.

### Optional requirements

- The "OpenSSH Client" optional feature (Settings, Apps, Optional features) is needed for everything under `sshm`.

## Starting out

Run `novashell.exe`. Type `help` for a short summary of everything below.

A few things worth knowing right away:

- Aliases are expanded before built-ins, so an alias can point to a built-in (the default `..` alias is `cd ..`).
- `cd` is a real built-in, so the directory change sticks. The same goes for `export` and `set NAME=value`.
- Lines starting with `#` are ignored. That is mostly useful in script files (because why would you use that in the terminal?).
- If input is piped into novaShell instead of typed, it reads plain lines without the line editor.

## The prompt

The default prompt looks like this:

```
[1] ~\projects\site (main)> 
```

- `[1]` is the exit code of the previous command. It is only shown when the code is not zero, in red. Large codes are shown in hex.
- The home folder is abbreviated to `~`.
- `(main)` is the current git branch, shown in a different color inside a git repository. For a detached HEAD, the short commit hash is shown.

### Changing the prompt

The `prompt` command sets the format and saves it to `prompt.txt`:

```
prompt "{user}@{host} {cwd}{git}{jobs}> "
prompt reset
prompt
```

Quotes are optional, but they are the way to keep a trailing space. Running `prompt` without arguments shows the current format.

| Token    | Meaning                                                         |
| -------- | --------------------------------------------------------------- |
| `{code}` | Red badge with the last exit code (empty when it was 0)         |
| `{cwd}`  | Current directory, with `~` for the home folder                 |
| `{git}`  | Current git branch in parentheses (empty outside a repository)  |
| `{time}` | Current time as `HH:MM:SS`                                      |
| `{user}` | User name                                                       |
| `{host}` | Computer name                                                   |
| `{jobs}` | Number of running background jobs, like ` [2 bg]`               |
| `{nl}`   | Line break, for a two-line prompt                               |

Unknown tokens are printed as they are. Setting the environment variable `NOVA_NOGIT=1` turns off the git lookup, which can help on slow network drives.

## Line editing

novaShell has its own line editor instead of the plain console input.

| Key                  | Action                                                         |
| -------------------- | -------------------------------------------------------------- |
| Left / Right         | Move the cursor                                                |
| Home / End           | Jump to start or end of the line                               |
| Ctrl+A / Ctrl+E      | Same as Home / End                                             |
| Ctrl+Left / Right    | Jump by word                                                   |
| Backspace / Delete   | Delete before or under the cursor                              |
| Ctrl+W               | Delete the previous word                                       |
| Ctrl+U or Esc        | Clear the whole line                                           |
| Up / Down            | Walk through history                                           |
| Ctrl+R               | Incremental history search (see below)                         |
| Tab                  | Complete; press again to cycle through candidates              |
| Right or End or Ctrl+E at the end of the line | Accept the ghost suggestion          |
| Ctrl+D on an empty line | Exit the shell                                              |

### Ghost suggestions

While typing, the most recent history entry that starts with the current text is shown dimmed after the cursor. Pressing Right, End or Ctrl+E at the end of the line accepts it. The suggestion is hidden while searching history and when Enter is pressed.

### History

- History is stored in `history.txt` and keeps the last 500 entries. Consecutive duplicates are not stored twice.
- `history [n]` prints the whole history or the last `n` entries.
- `!!` runs the previous command again, `!N` runs entry number `N` from the `history` listing. The expanded line is echoed before it runs.
- A line that starts with a space is not saved to history. That could be handy for commands with secrets in them.
- `history-clear` wipes the history.

### Ctrl+R search

Press Ctrl+R and start typing. The newest entry containing the typed text (case-insensitive) is shown. Ctrl+R again jumps to the next older match. Enter or an arrow key puts the match on the line without running it, and Esc cancels.

## Tab completion

Completion depends on where in the line the cursor is.

- **First word:** built-in commands, aliases and executables found on `PATH` (`.exe`, `.bat`, `.cmd`).
- **Paths:** files and folders, with `~` and `%VAR%` expanded. Hidden dot-files only show up when the typed fragment starts with a dot. After `cd` only directories are offered.
- **Environment variables:** after an unclosed `%`, variable names are completed (`echo %PATH%`). After `export`, `set` and `unset`, variable names are offered too.
- **git:** subcommands after `git`, and branches, remote branches and tags after subcommands like `checkout`, `switch`, `merge`, `rebase`, `branch`, `diff`, `log`, `reset`, `show` and `cherry-pick`. Names with slashes such as `feature/login` work. Files are still offered next to the refs.
- **ssh, sftp, scp:** host names from `~\.ssh\known_hosts` (unhashed entries only) and `Host` aliases from `~\.ssh\config`. Wildcard hosts are skipped. `user@ho<Tab>` keeps the user part. For `scp`, local files are offered as well.
- **sshm and bm:** subcommands and profile or bookmark names. See the sections below.
- **alias, unalias:** alias names.

When several candidates match, the common prefix is filled in first and the candidates are listed in columns. Pressing Tab again cycles through them.

## Command reference

### Navigation

| Command                     | What it does                                                         |
| --------------------------- | -------------------------------------------------------------------- |
| `cd [path]`                 | Change directory. Without arguments it prints the current directory. |
| `cd -`                      | Go back to the previous directory.                                   |
| `cd ~\Documents`            | `~` stands for the home folder.                                      |
| `pwd`                       | Print the current directory.                                         |
| `mkcd <path>`               | Create a directory (including missing parents) and enter it.         |
| `dir [path or pattern]`     | Colored directory listing.                                           |

`cd` also understands the `cmd.exe` habits `cd /d X`, `cd..` and `cd\`.

`dir` accepts a folder, a single file or a wildcard such as `dir *.txt`. Folders are cyan, executables green and archives red. If the arguments start with a switch (`dir /b`, `dir /s`) or the line contains a pipe or redirect, the real `cmd.exe` `dir` is used instead.

### Bookmarks

Bookmarks are named directories, stored in `bookmarks.txt`.

```
bm add proj C:\code\project    save a bookmark (without a path it uses the current directory)
bm proj                        jump to it
bm                             list bookmarks
bm rm proj                     delete a bookmark
```

Bookmark names may contain letters, digits, `.`, `_` and `-`.

### Environment

| Command                | What it does                                                            |
| ---------------------- | ----------------------------------------------------------------------- |
| `export NAME=value`    | Set an environment variable. `%VAR%` in the value is expanded.          |
| `set NAME=value`       | Same as `export`. Plain `set` still lists variables through `cmd.exe`.  |
| `unset NAME`           | Remove a variable.                                                      |
| `env`                  | List all variables.                                                     |

Variables set this way are visible to every program started afterwards, and they last until the shell exits. To make them permanent, put the commands into `startup.txt`.

### Aliases

```
alias                    list all aliases
alias ll                 show one alias
alias ll dir /b          define (or update) an alias
alias ll="dir /b"        same, with = syntax
unalias ll               remove it
```

Aliases are saved to `aliases.txt` right away. On the very first run a few defaults are created (`ls`, `list`, `ll`, `clr`, `..`, `...`, `cp`, `mv`, `rm`). After that the file is the only source of truth, so a removed default stays removed.

An alias only replaces the first word of a line, and the rest of the line is appended. An alias that refers to itself (`ls=ls -la`) is expanded once and not repeatedly.

#### Alias arguments

If the alias text contains `$1` to `$9` or `$*`, the typed arguments are inserted at those places and nothing is appended automatically.

| Placeholder | Meaning                                    |
| ----------- | ------------------------------------------ |
| `$1`..`$9`  | The first to ninth argument                |
| `$*`        | All arguments, as typed                    |
| `$$`        | A literal dollar sign (but why would you?) |

```
alias mkproj=mkcd ~\code\$1 && git init
mkproj webapp
```

Missing arguments become empty text. Arguments containing spaces should be quoted when typing.

### Scripting

| Command           | What it does                                                                          |
| ----------------- | ------------------------------------------------------------------------------------- |
| `source <file>`   | Run the commands in a text file, one per line. Empty lines and `#` lines are skipped. |
| `time <command>`  | Run a command and print how long it took.                                             |
| `touch <files>`   | Create files, or update the timestamp of existing ones.                               |

#### startup.txt

`startup.txt` in the configuration folder is run like a `source` script every time novaShell starts. It is a good place for `export` lines, `alias` lines or a `cd`.

#### .novarc

When `cd` (or `bm`, or `mkcd`) enters a folder that contains a file named `.novarc`, novaShell can run it. Because that is code running just from entering a folder, it asks first:

```
Run this .novarc?  [y] once   [a] always (this version)   [N] no:
```

- `y` runs it this time only.
- `a` trusts this exact content. A hash of the file is stored in `trusted_rc.txt`, so as soon as the file changes, novaShell asks again.
- Anything else skips it.

The same applies to the folder novaShell is started in. Setting `NOVA_NORC=1` disables the feature. A `.novarc` is a normal script, so `export`, `alias` and `source venv\Scripts\activate.bat`-style lines work. Variables it sets are not undone when leaving the folder.

### System

| Command              | What it does                                                                 |
| -------------------- | ---------------------------------------------------------------------------- |
| `sudo <command>`     | Run a command elevated. A UAC prompt appears, the command runs in a new window that waits for a key press at the end, and novaShell continues when it is closed. |
| `sudo`               | Open a new elevated novaShell.                                               |
| `which <name>`       | Say whether a name is an alias, a built-in or an executable (with its path). |
| `title <text>`       | Set the console window title.                                                |
| `clear`, `cls`       | Clear the screen.                                                            |
| `help`               | Print the built-in summary.                                                  |
| `about`              | Version information.                                                         |
| `exit`, `quit`       | Leave the shell.                                                             |

### config

The `config` command helps with the configuration files.

```
config                   show the folder and the file names
config open              open the folder in Explorer
config edit              edit startup.txt in Notepad
config edit aliases      also: ssh, bookmarks
config reload            re-read aliases, ssh profiles and bookmarks after editing by hand
```

### Text tools

`cat`, `head`, `tail`, `wc` and `grep` are built in, so they exist on any machine. They read the files given as arguments (wildcards like `*.log` work), or standard input if no file is given.

| Command                  | Details                                                                      |
| ------------------------ | ---------------------------------------------------------------------------- |
| `cat [-n] [files]`       | Print files. `-n` numbers the lines.                                         |
| `head [-n N] [files]`    | First 10 lines, or `N` lines. `-5` is short for `-n 5`.                      |
| `tail [-n N] [-f] [files]` | Last 10 lines, or `N`. `-f` keeps following a single file until Ctrl+C.   |
| `wc [-l] [-w] [-c] [files]` | Count lines, words and bytes. Without flags all three are shown.          |
| `grep [options] pattern [files]` | Search for text, see below.                                          |

With more than one file, `head` and `tail` print a `==> name <==` header per file. `-q` hides the headers.

#### grep

`grep` does a plain substring search. Regular expressions are not supported.

| Option | Meaning                                                  |
| ------ | -------------------------------------------------------- |
| `-i`   | Ignore case                                              |
| `-v`   | Show lines that do not match                             |
| `-n`   | Show line numbers                                        |
| `-c`   | Only print the number of matching lines per file         |
| `-l`   | Only print the names of files that contain a match       |
| `-r`   | Search folders recursively. Without a file argument the current folder is searched. |
| `-H`   | Always print the file name                               |
| `-e`   | Take the next argument as the pattern (for patterns starting with `-`) |

Options can be combined, like `-rin`. File names are shown automatically when more than one file is searched. Matches are highlighted when output goes to the console. The exit code is 1 when nothing matched, which shows up in the prompt badge and works with `&&`.

```
grep -rin "todo" *.c
dir | grep txt
type log.txt | grep -i error | wc -l
```

## Chaining, pipes and background jobs

### Chaining with &&

`a && b` runs `b` only if `a` succeeded. novaShell handles this itself, so built-ins can be part of a chain (`cd build && make`). Quoted `&&` is left alone. A line containing parentheses is passed to `cmd.exe` untouched, so constructs like `if exist x (a && b)` still work.

### Pipes and redirects

Pipes and redirects are done by `cmd.exe`. The text tools are built into novaShell, which `cmd.exe` knows nothing about. To make them work inside a pipeline, novaShell rewrites the first word of each pipeline segment: `dir | grep txt` is run as `dir | "C:\path\novashell.exe" --nova-exec grep txt`. A small child instance of novaShell then runs the tool with the pipe as input and output. This is invisible in normal use.

### Background jobs

A line ending in a single `&` runs in the background:

```
ping -t example.com &
sshm tunnel web 8080:localhost:80 &
```

Built-ins can be backgrounded too, because a child novaShell is used for them.

| Command     | What it does                                                                       |
| ----------- | ---------------------------------------------------------------------------------- |
| `jobs`      | List jobs with their number, state and process id.                                 |
| `fg [n]`    | Wait for a job (the newest one without an argument). Ctrl+C stops waiting and leaves the job running. |
| `kill %n`   | Stop job `n` including everything it started.                                      |
| `kill <pid>`| Stop any process by id.                                                            |

Details:

- On start, the shell prints `[1] 12345` (job number and process id). When a job finishes, a `[1]+ Done (exit 0)` line appears before the next prompt.
- Background jobs get no keyboard input and ignore Ctrl+C typed at the prompt.
- Their output goes to the same console, so it can show up in the middle of typing.
- Jobs are tied to novaShell. When the shell exits, they are stopped.

## Exit status and notifications

### Status line

After every external command (anything handed to `cmd.exe`) a status line is printed:

| Output                                                      | Color  |
| ----------------------------------------------------------- | ------ |
| `Process exited with code 0 (Success)`                      | green  |
| `Process exited with code 1 (General Error)`                | red    |
| `Process exited with code 0xC000013A (Process terminated by CTRL-C)` | red |
| `Process exited with code 5` (any other code, no meaning)   | yellow |

Built-ins do not print this line. It is meant to be extended: new meanings can be added in the `print_exit_status` function in the source.

### Slow commands

If an external command takes 30 seconds or longer, novaShell prints how long it took (for example `Took 1m 23s`) and rings the terminal bell. Windows Terminal turns the bell into a taskbar flash. The threshold can be changed with the environment variable `NOVA_NOTIFY_SECS`. Setting it to 0 turns the feature off.

## SSH profiles

`sshm` stores connections that are used often, so a long `ssh` line turns into a short name. It uses the OpenSSH client (`ssh.exe` and `scp.exe`) that comes with Windows. Profiles are stored in `ssh_hosts.txt`.

### Saving and using profiles

```
sshm add web admin@example.com -p 2222 -i ~\.ssh\id_ed25519
sshm list
sshm web
sshm web uptime
```

`sshm add <name> [user@]host` takes these options:

- `-p <port>` sets the port.
- `-i <keyfile>` sets the identity file (`~` is expanded).
- Everything else is stored as extra ssh options and added on every connection, for example `-L 8080:localhost:80` or `-o ServerAliveInterval=30`.

Adding a name that already exists updates it. `sshm <name>` (or `sshm connect <name>`) connects, and extra words after the name are run as a remote command.

### All subcommands

| Command                                   | What it does                                                     |
| ----------------------------------------- | ---------------------------------------------------------------- |
| `sshm list`                               | Show all profiles (also what plain `sshm` does)                  |
| `sshm add <name> [user@]host [options]`   | Save or update a profile                                         |
| `sshm connect <name> [command...]`        | Connect, optionally running a command                            |
| `sshm last [command...]`                  | Connect to the profile that was used most recently               |
| `sshm show <name>`                        | Show the profile and the exact command line that will run        |
| `sshm rename <old> <new>`                 | Rename a profile                                                 |
| `sshm rm <name>`                          | Delete a profile                                                 |
| `sshm test <name>`                        | Check that the host is reachable and key login works             |
| `sshm tunnel <name> [-L\|-R\|-D] <spec>`  | Open a port forward without a shell                              |
| `sshm copy-id <name> [pubkey]`            | Install a public key on the server                               |
| `sshm import [file]`                      | Import profiles from an OpenSSH config file                      |
| `sshm put <name> [-r] <local> <remote>`   | Upload with scp                                                  |
| `sshm get <name> [-r] <remote> <local>`   | Download with scp                                                |

### Details

- **test** runs `ssh` in batch mode with a 5 second timeout and does not ask for passwords. A host that only accepts passwords therefore reports a failure, even though it is reachable.
- **tunnel** defaults to a local forward (`-L`). The spec is the usual ssh syntax, like `8080:localhost:80`. `-D 1080` opens a SOCKS proxy. The tunnel stays open until Ctrl+C. Putting `&` at the end runs it as a background job.
- **copy-id** pipes the public key into a small script on the server that appends it to `~/.ssh/authorized_keys` and skips it if it is already there. The password is asked for once. It is meant for Linux and Unix servers. Without an explicit file, it uses the profile's key with `.pub` added, then `id_ed25519.pub`, then `id_rsa.pub` from `~\.ssh`.
- **import** reads `~\.ssh\config` by default. It picks up `HostName`, `User`, `Port` and `IdentityFile` from `Host` blocks. Wildcard patterns and `Match` blocks are skipped, and profiles that already exist are never overwritten.
- **put** and **get** pass the port and key of the profile to scp. Flags starting with `-` (like `-r`) go straight to scp.

### Safety

Profile names, user names and host names are checked before use (letters, digits and a few separators, never starting with `-`), and ssh is started directly instead of through `cmd.exe`. This is intended because it keeps a mistyped or hand-edited profile from turning into an option or a shell command. Lines in `ssh_hosts.txt` that fail these checks are ignored, and a warning with the count is printed at startup.

## Configuration

Everything lives in `%APPDATA%\novaShell`. `config` shows the exact path.

| File               | Purpose                                                                   |
| ------------------ | ------------------------------------------------------------------------- |
| `aliases.txt`      | One `name=command` per line                                               |
| `startup.txt`      | Commands run at every start                                               |
| `history.txt`      | Command history, one entry per line                                       |
| `ssh_hosts.txt`    | SSH profiles, tab-separated fields                                        |
| `ssh_last.txt`     | Name of the most recently used SSH profile                                |
| `bookmarks.txt`    | Directory bookmarks, tab-separated                                        |
| `prompt.txt`       | Saved prompt format                                                       |
| `trusted_rc.txt`   | Hashes of `.novarc` files that were trusted                               |

All files are plain text and can be edited by hand. After editing aliases, ssh profiles or bookmarks while the shell is running, `config reload` picks up the changes.

### Environment variables

| Variable             | Effect                                                                      |
| -------------------- | --------------------------------------------------------------------------- |
| `NOVA_NOGIT`         | Set to anything to skip the git branch lookup                               |
| `NOVA_NORC`          | Set to anything to ignore `.novarc` files                                   |
| `NOVA_NOTIFY_SECS`   | Seconds a command must run before the bell and "Took ..." line (default 30, 0 turns it off) |

## Limits and known quirks

- The line is limited to 4096 characters. There is room for 500 history entries, 64 aliases, 128 SSH profiles, 64 bookmarks and 32 background jobs.
- `grep` matches plain text, not regular expressions.
- The text tools read files in text mode, so `wc -c` counts after line ending conversion and can differ from the real file size on files with Windows line endings.
- The line editor counts one character as one console column. Wide characters (some CJK text, emoji) can throw the cursor position off.
- Ctrl+C at the prompt does nothing. Ctrl+C while a command runs goes to that command.
- Background job output is written straight to the console, so it can interleave with the prompt.
- `cd` without arguments prints the current directory (like `cmd.exe`) instead of jumping home. `cd ~` does the latter.
- Only a trailing single `&` starts a background job. `a & b` in the middle of a line is handled by `cmd.exe`.
- `.novarc` changes to the environment are not reverted when leaving the folder.
- `sshm copy-id` assumes a Unix-like server. Windows OpenSSH servers keep authorized keys in a different place.

## Author

Annabeth Kisling

[annabeth@tk-dev-software.com](mailto:annabeth@tk-dev-software.com)

[tk-dev-software.com](tk-dev-software.com)
