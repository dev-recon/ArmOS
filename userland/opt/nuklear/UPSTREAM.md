# Upstream

- Project: Nuklear
- Version: 4.13.3
- Git tag: `v4.13.3`
- Commit: `a53ad2c658151071501372a5e0e5e978153835aa`
- Source: https://github.com/Immediate-Mode-UI/Nuklear
- Imported header SHA-256:
  `2e3b7c3f6528cd8e43aeae87e343fe9ca529b6f7380a99ebb63a41cf1d4a1552`
- License choice for ArmOS: MIT

The import intentionally keeps the upstream single header unchanged. ArmOS
configuration lives in `src/nuklear.c`; Wayland, input and software-rendering
integration lives in ArmOS-owned code outside this third-party bundle.
