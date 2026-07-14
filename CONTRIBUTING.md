# Contributing

## How to Contribute

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-idea`)
3. Make your changes
4. Build: `make`
5. Test: `sudo ./scripts/run_tests.sh [ssd|hdd|all]`
6. Commit and push
7. Open a Pull Request

## Code Style

- Linux kernel coding style
- No comments in code (self-documenting variable names)
- No trailing whitespace
- 8-character tabs for indentation

## Testing

Always run at least the SSD benchmark before submitting:

```bash
sudo ./scripts/run_tests.sh ssd
```

Include benchmark results in your PR description.
