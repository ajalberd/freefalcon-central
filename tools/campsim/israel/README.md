# Israel campaign tooling (ITO theaters)

Scripts used to repair, rebuild and retune the nine Israel campaigns in `C:\FreeFalcon6\Theaters\{Israel,Israel2007,Israel_Classic}`.
They edit the **install**, keeping every original (`*.bak-orig` = shipped files, `*.bak-pre-retune` = before the retune).
Findings and the traps behind them: `../README.md` ("Other theaters" / "The Israel theaters") and `../../../CAMPAIGN-AI-FINDINGS.md`.

| script | what it does |
|---|---|
| `fix_tri.py [--write]` | the original repair of the shipped `.tri` scripts: `#ELSE`/`#ENDIF` structure, toggles, remapped objective IDs, trailing-space `#ENDIF`, Classic's swapped save0/save2 scripts |
| `build_modern.py [--write]` | rebuilds Israel 2012's hidden `save0` / `save1` as the 2004 and 2012 Fictional Wars (copy of the 2007 save, era aircraft per squadron, own `tri_save0.tri` / `tri_save1.tri`, Arab ground cut) |
| `retune.py` | retunes the seven older campaigns toward a 5-10 day war: end-condition lists, day limits, a minimum day before an ending, ground cuts. Idempotent: always starts from the `.bak-pre-retune` copies |
| `report.py` | one line per finished run in `../runs`: end code, its label from the script comments, day, team strengths |

Run a campaign with `python ../run_batch.py --theater israeli --save save1 --days 10 --seeds 1 2 3 4 --set usecfg=1`
(`--theater list` shows the stems). Remember that the engine re-reads the `.tri` on every check: stop runs before editing one.
