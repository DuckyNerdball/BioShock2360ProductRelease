# bioshock2mp STATUS (append-only: declared functions and why)
- 2026-11-07 - project created from bioshock2-recomp tooling; hooks: ProcessEvent 828209B0, CallFunction 82821AC0 (signature-matched from SP)
- 2026-11-07 - 0x82914158, 0x8287DEC8: tail-call targets, first codegen UnresolvedCall (pdata gaps)
- 2026-11-07 - vtable-scan bulk import: 96 functions declared in [entrypoint.functions] (import_functions)
- 2026-11-07 - 0x828EF9B0 - runtime: call to unregistered function - declared in [entrypoint.functions] (fix_loop)
- 2026-11-07 - SDK cvar fake_live_signin (xam_user.cpp): MP front end said 'You must be signed into Xbox LIVE'; go.ps1 passes it
- 2026-11-07 - bot aim v2: aim from the bot's eye (Pawn.EyePosition, EYE lines), error in units at the target (aimmiss), BOTSTATS every 10 s
- 2026-11-07 - bot spawn spread (ModifyPlayerPosition out-Location rewrite, BOTSPAWN), per-bot BOTS line
- 2026-11-07 - bot names (FString operator= sub_8249E720 on PRI+0x490, PRI = ctrl+0x470), hunters respawn 1200-3000 from you
