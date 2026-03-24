# Contributing to Pulp

We welcome contributions. Here's how to get started.

## Developer Certificate of Origin

All contributions must be signed off under the [Developer Certificate of Origin](https://developercertificate.org/) (DCO). This certifies that you have the right to submit the code under the project's MIT license.

Add `Signed-off-by: Your Name <your@email.com>` to your commit messages, or use `git commit -s`.

## Code Standards

- **C++20** — use modern features where they improve clarity
- **Original naming** — all names must be original to Pulp
- **Platform isolation** — platform-specific code goes in `platform/` subdirectories
- **Tests required** — every feature needs tests
- **Clean commits** — imperative mood, explain why not just what

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

## Dependencies

Before adding any dependency:
1. Check its license (MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0 only)
2. Add it to `DEPENDENCIES.md` (alphabetical order)
3. Add its license text to `NOTICE.md` (alphabetical order)
