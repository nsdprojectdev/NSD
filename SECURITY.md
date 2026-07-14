# Security Policy

## Reporting a Vulnerability

NSD operates as a kernel module with kprobe hooks.
If you discover a security vulnerability, please report it by
opening an issue at https://github.com/anomalyco/nsd/issues.

Do NOT disclose kernel-level vulnerabilities publicly until
a fix has been released and verified.

## Scope

- Kernel memory safety (kprobe, ring buffers, workqueues)
- Privilege escalation via sysfs interface
- Information disclosure through stats/telemetry
