# PROTOTYPE — language switch placement

Throwaway. Answers [Where does the language switch live, and what does it look like?](https://github.com/peilinok/winaudio/issues/136). Not product code.

Three variants of the language switch on a mock of the existing WinAudio window, switchable via `?variant=`.

```
start src\gui\prototype-language-switch\index.html
```

- A — Tab-bar combo: `index.html?variant=A`
- B — Left-panel radios: `index.html?variant=B`
- C — Log-toolbar combo: `index.html?variant=C`

Arrow keys and the floating bar cycle variants. Switching English / 中文 is in-memory only (storage is a different ticket).
