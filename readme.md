# novaShell

![C Language](https://img.shields.io/badge/Written_in-C-blue?logo=c) 
![Windows](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows) 
![Version](https://img.shields.io/badge/Version-1.0-green)  
![License](https://img.shields.io/badge/License-MIT-yellow)

---

## Overview
**novaShell** is a Windows terminal wrapper written in **C**, featuring **tab autocomplete**, **Linux-style aliases**, **improved command history**, and **enhanced readability through colored output**.  
The long-term goal is to develop a **standalone GUI version in C++**.

---

## Features

| 🚀 Feature         | Description                                                                 |
|--------------------|-----------------------------------------------------------------------------|
| 🔹 **Tab Autocomplete** | Type a shortcut and press `Tab` to expand it into a full command.           |
| 🐧 **Linux Aliases**    | Use familiar Linux commands, automatically converted to Windows equivalents.|
| ⏳ **Command History**  | Use arrow keys to navigate history or run `nvs-history` to list all.        |
| 🎨 **Colored Output**   | Improves readability of terminal responses.                                 |
| ⚙️ **Built-in Commands**| Enhanced internal handling of `cd`, `dir`, and others.                      |
| 🧩 **Customizable**     | Easily extend autocomplete and alias features in `main.c`.                  |

---

## How to Use
After downloading and/or compiling the software, simply run the `.exe` file.  
A Windows terminal will open, mimicking the normal command line but with additional features like:
- Tab autocomplete for common commands
- Linux-to-Windows command aliases
- Improved command history
- Built-in `dir` and `cd` functions
- Colored output for better readability

---

## Autocomplete
To use autocomplete, type a shortcut and press `Tab`.  
The following autocomplete shortcuts are currently implemented (more are planned):

### General
| Shortcut | Expands To        |
|----------|-------------------|
| /        | //                |
| v        | ver               |
| p        | pwd               |
| h        | help              |
| r        | rmdir             |
| m        | move              |
| c        | cls               |
| d        | del               |
| ex       | explorer .        |
| mk       | mkdir             |
| co       | copy              |
| ip       | ipconfig          |
| ..       | cd ..             |
| di       | dir               |
| sys      | systeminfo        |
| pingg    | ping google.com   |
| kill     | taskkill /PID     |

### Internal
| Shortcut | Expands To    |
|----------|---------------|
| nvs      | nvs-help      |
| about    | nvs-about     |
| history  | nvs-history   |

**Adding Custom Shortcuts:**  
To create new shortcuts, simply add them to the `complete_command` function in `main.c`.

---

## Aliases
For users accustomed to Linux commands, novaShell automatically converts certain Linux commands to their Windows counterparts.

| Linux Command | Windows Equivalent |
|---------------|--------------------|
| ls            | dir                |
| list          | dir                |
| clr           | cls                |
| date          | time               |
| pwd           | chdir              |

**Adding Custom Aliases:**  
To add more aliases, include them in the `aliases` struct in `main.c`.

---

## History
- Use the **Up/Down arrow keys** to cycle through recent commands.
- Enter `nvs-history` to output all previously used commands.
- Close the terminal to clear the command history.

---

<details>
<summary>⚡ **Advanced Usage & Customization**</summary>

### Adding New Autocomplete Shortcuts
1. Open `main.c`.
2. Locate the `complete_command` function.
3. Add your custom shortcut and expansion mapping.

### Adding New Aliases
1. Open `main.c`.
2. Locate the `aliases` struct.
3. Add your custom Linux-to-Windows alias pair.

---

### Planned Features
- A full **C++ standalone GUI**.
- **Plugin support** for user-defined scripts.
- **Persistent command history** across sessions.
- **Custom color schemes**.

</details>
