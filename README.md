# amiga-ssh

An SSH server for AmigaOS 3.x on 68k Amigas. It is the Dropbear SSH server,
adapted to AmigaOS: you can log in from any SSH client, get an AmigaDOS
shell, or run single commands (`ssh amiga@<address> list`) and get the
command's return code back. Several sessions can run at once. How the port
works is described in [docs/DESIGN.md](docs/DESIGN.md).

## Requirements

- AmigaOS 3.x.
- A 68000 or any later 68k CPU. The programs are built with the
  m68k-amigaos-gcc toolchain's default CPU setting, which is 68000
  (`m68k-amigaos-gcc -Q --help=target` reports `-mcpu= 68000`), so they
  use no 68020-only instructions.
- A TCP/IP stack that provides `bsdsocket.library` version 4 or later
  (the AmiTCP programming interface). Tested with Roadshow on AmigaOS 3.2.2.
- Fast RAM is recommended. The server never asks for chip RAM (see
  docs/DESIGN.md, Memory).

## Installing

1. Copy the `dist` directory to the Amiga (for example to `RAM:` or a
   hard disk).
2. Open a Shell, `CD` into that directory and type:

   ```
   Execute Install-SSH
   ```

The installer first checks that a TCP/IP stack is running and stops,
changing nothing, if it is not. It asks no questions. It copies the
programs to `C:`, creates `DEVS:Internet/sshd` with the config, an empty
`authorized_keys` and the host keys, and adds a start command to
`S:User-Startup`, so the server starts at every boot. Generating the host
keys takes a while, the RSA key the longest.

To start the server right away without rebooting:

```
Run >NIL: C:sshd
```

Running `Install-SSH` again updates the programs and keeps the config,
the password, the host keys and `authorized_keys`.

## First login

From another computer on the same network:

```
ssh amiga@<address of the Amiga>
```

The login name is `amiga` and the password is `amiga`.

You get an AmigaDOS shell. Leave it with `EndShell`, or with `exit` or
`logout`, which SSH sessions add as aliases (see
`DEVS:Internet/sshd/shell-startup`, which runs your `S:Shell-Startup` first).

## Copying files (SFTP)

Any SFTP client works, for example from a Mac or Linux:

```
sftp amiga@<address of the Amiga>
```

The Amiga appears as one tree: `/` lists the mounted volumes and the
assigns, `/Work/Projects` is `Work:Projects`, and a session starts in the
`home=` directory (`SYS:` by default). Upload with `put`, download with
`get`. OpenSSH 9.0 and later send `scp` through SFTP too, so
`scp file amiga@<address>:/RAM/` copies to `RAM:` (not tested yet).
Graphical SFTP clients (FileZilla, Cyberduck, WinSCP) use the same
protocol but have not been tested.

Measured with OpenSSH's `sftp` on an A2000 with PiStorm, after login: 1 MB
up in about 4 seconds, 1 MB down in about 2.5 seconds.

## Changing the password

On the Amiga, type

```
sshpasswd
```

It asks for the new password twice without showing it. `sshpasswd
<password>` sets it without asking. Only a SHA-256 crypt hash of the
password is stored (the `$5$` scheme, the same as `openssl passwd -5`).

## Changing the login name

Edit the `user=` line in `DEVS:Internet/sshd/config` with any text editor
(for example `Ed DEVS:Internet/sshd/config`). The `home=` line in the same
file is the directory a session starts in (default `SYS:`).

## Logging in with a key

Add your public key (the contents of `~/.ssh/id_ed25519.pub` or similar
on your computer, one key per line) to

```
DEVS:Internet/sshd/authorized_keys
```

Every key in that file logs in as the one account.

## Files

| File | Purpose |
|---|---|
| `C:sshd` | the server |
| `C:sshd-console` | the console handler that connects a shell to an SSH session |
| `C:sshpasswd` | sets the login password |
| `C:dropbearkey` | generates host keys |
| `C:sftp-server` | the SFTP file transfer server `sshd` starts for `sftp` and `scp` |
| `C:sshd-startup` | adds or removes the start command in `S:User-Startup` (used by the install scripts) |
| `DEVS:Internet/sshd/config` | login name (`user=`), home directory (`home=`), password hash (`password=`) |
| `DEVS:Internet/sshd/authorized_keys` | public keys allowed to log in |
| `DEVS:Internet/sshd/dropbear_ed25519_host_key`, `dropbear_ecdsa_host_key`, `dropbear_rsa_host_key` | the server's host keys |
| `DEVS:Internet/sshd/random_seed` | saved state of the random number pool |
| `DEVS:Internet/sshd/log`, `log.old` | the server's log: connections, logins and session ends; at 16 KB `log` becomes `log.old`, so both stay small |
| `DEVS:Internet/sshd/shell-startup` | start-up script of SSH shells: runs `S:Shell-Startup`, adds `exit` and `logout` |
| `S:User-Startup` | holds the start command between the lines `;BEGIN amiga-ssh` and `;END amiga-ssh` |

## Uninstalling

In a Shell, in the `dist` directory:

```
Execute Uninstall-SSH
```

It removes the start command from `S:User-Startup` (every other line there
stays) and the programs from `C:`. It then asks whether to delete
`DEVS:Internet/sshd` too; pressing Return keeps it, so a later install
reuses the same host keys and password. A server that is still running
stops at the next reboot.

## Limitations

- SFTP: symbolic links can be listed but not created or read
  (`readlink`/`symlink` are not supported), and files must stay under
  2 GB (AmigaOS 3.x file positions are 32-bit).
- For an interactive shell the exit status the client receives is that of
  `NewShell`, not of the last command typed. `ssh amiga@<address> <command>`
  returns the command's real return code.
- No environment variables (such as `TERM`) are passed to the shell.
- The terminal size the client sends is stored, but programs on the Amiga
  cannot ask for it yet.
- Port forwarding is built in but has not been tested.
- A connection that a client keeps open (for example through OpenSSH's
  `ControlMaster`/`ControlPersist` connection sharing) keeps its session
  process alive after `exit` until the client closes it; that is how SSH
  works, not a fault of the server.

## Security notes

- **The default password is `amiga`.** Anyone who can reach the Amiga's
  port 22 can log in with it until you change it with `sshpasswd`. Change
  it right after installing.
- Use the server on a local network only. Do not make port 22 reachable
  from the internet: the machine has one account with full access to the
  whole system, and AmigaOS has no memory protection or user separation.
- Randomness: AmigaOS has no random number device. The server keeps an
  entropy pool in `DEVS:Internet/sshd/random_seed`, adds timer jitter,
  the arrival time of each connection and system state to it, and hashes
  everything with SHA-256. How much real entropy these sources give depends
  on the machine. The installer generates the host keys with whatever the
  pool holds at that moment, without asking for any typing or mouse
  movement. If you want host keys from a stronger source, generate them on
  another computer with Dropbear's `dropbearkey` and copy them into
  `DEVS:Internet/sshd/` (see BUILDING.md).

## Building from source

See [BUILDING.md](BUILDING.md). The build fetches Dropbear at a pinned
commit and applies the port at build time; Dropbear's code is not copied
into this repository.

## License

Copyright (c) 2026 zombu2.

- The code of this project (everything in `port/`, the install scripts and
  the build files) is licensed under the PolyForm Noncommercial License
  1.0.0: it is free for any noncommercial use, and it may not be sold or
  bundled with a commercial product. See [LICENSE](LICENSE) (also shipped
  as `dist/LICENSE`).
- Dropbear is under the MIT license; its license file is shipped as
  `dist/LICENSE.dropbear`. It refers to the license files of LibTomCrypt
  and LibTomMath, which Dropbear includes; they are shipped as
  `dist/LICENSE.libtomcrypt` and `dist/LICENSE.libtommath`.

## Credits

- Dropbear SSH, by Matt Johnston and contributors.
- LibTomCrypt and LibTomMath, by Tom St Denis and others, which Dropbear
  includes; both are public domain according to their own license files
  (LibTomCrypt offers the WTFPL as a second choice).
- telnetd for AmigaOS by Tor Anders Johansen
  (github.com/toralux/amiga-telnetd), whose way of starting shells was the
  model for this port's process handling. No code was taken from it.
- The AmigaPorts m68k-amigaos-gcc toolchain and its libnix C library.
