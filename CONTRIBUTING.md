# Contributing to Helix

Thank you for your interest in contributing.

## Development

Clone repository:

```bash
git clone https://github.com/oss-helix/helix.git
```

### Branch

Create a feature branch:

```bash
git checkout -b feature/my-feature
```

### Commit Style

Use clear commit messages.

Examples:

- Add event loop scheduler
- Fix state routing bug
- Improve memory allocator

### Pull Requests

All changes should be submitted through Pull Requests.

A PR should include:

- description
- motivation
- testing information

### Code Style

Prefer:

- simple designs
- explicit memory ownership
- predictable performance

Avoid:

- unnecessary abstraction
- hidden allocations
- blocking operations inside event loops

## Discussions

For large changes:

Open an issue first.

Explain:

- problem
- proposed solution
- tradeoffs

## License

By contributing, you agree that your contributions are licensed under
Apache License 2.0.
