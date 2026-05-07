# Security Policy

FreezerManager is designed for labs that may handle sensitive biospecimen data,
including PHI. Treat security issues and privacy leaks as high priority even
while the project is pre-alpha.

## Reporting a Vulnerability

Email reports to Yuxin Ren at `yxren_CN@outlook.com`. Do not open a public
issue for suspected vulnerabilities, PHI exposure, authentication bypasses,
authorization failures, cryptographic mistakes, or data-loss risks.

Include:

- Affected component or file path.
- Reproduction steps or proof of concept.
- Expected and observed behavior.
- Any logs with secrets and PHI removed.

Encrypted reporting details will be added when a project GPG key is available.

## Disclosure Process

The project follows a 90-day coordinated disclosure target. The maintainer will
acknowledge reports when received, investigate impact, prepare a fix when
needed, and coordinate public disclosure after a patch or mitigation is ready.

## Handling Sensitive Data

Never include real PHI, access tokens, passwords, private keys, or production
database content in issues, PRs, commits, fixtures, screenshots, logs, or crash
reports. Redact sample identifiers unless they are synthetic.
