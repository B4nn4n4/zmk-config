#pragma once
#include <dt-bindings/zmk/keys.h>

/*
 * Shared alpha definitions for QWERTY / Colemak (plain, not DH).
 * Geometries differ (Hatsu 52 vs Zen 42 vs Tractyl 30+thumbs) so we
 * expose per-row letter macros, not full bindings vectors.
 *
 * Colemak plain:
 *   Q W F P G | J L U Y SEMI
 *   A R S T D | H N E I O
 *   Z X C V B | K M CMMA DOT FSLH
 * QWERTY:
 *   Q W E R T | Y U I O P
 *   A S D F G | H J K L SEMI
 *   Z X C V B | N M CMMA DOT FSLH
 *
 * Individual keycode macros (no &kp prefix) — allows wrapping with
 * &bhm for homerow mods: &bhm LSHIFT QW_HRML_A etc.
 */

/* ── QWERTY letters ── */
#define QW_R1L1 Q
#define QW_R1L2 W
#define QW_R1L3 E
#define QW_R1L4 R
#define QW_R1L5 T
#define QW_R1R1 Y
#define QW_R1R2 U
#define QW_R1R3 I
#define QW_R1R4 O
#define QW_R1R5 P

#define QW_R2L1 A
#define QW_R2L2 S
#define QW_R2L3 D
#define QW_R2L4 F
#define QW_R2L5 G
#define QW_R2R1 H
#define QW_R2R2 J
#define QW_R2R3 K
#define QW_R2R4 L
#define QW_R2R5 SEMI

#define QW_R3L1 Z
#define QW_R3L2 X
#define QW_R3L3 C
#define QW_R3L4 V
#define QW_R3L5 B
#define QW_R3R1 N
#define QW_R3R2 M
#define QW_R3R3 CMMA
#define QW_R3R4 DOT
#define QW_R3R5 FSLH

/* ── Colemak (plain) letters ── */
#define CM_R1L1 Q
#define CM_R1L2 W
#define CM_R1L3 F
#define CM_R1L4 P
#define CM_R1L5 G
#define CM_R1R1 J
#define CM_R1R2 L
#define CM_R1R3 U
#define CM_R1R4 Y
#define CM_R1R5 SEMI

#define CM_R2L1 A
#define CM_R2L2 R
#define CM_R2L3 S
#define CM_R2L4 T
#define CM_R2L5 D
#define CM_R2R1 H
#define CM_R2R2 N
#define CM_R2R3 E
#define CM_R2R4 I
#define CM_R2R5 O

#define CM_R3L1 Z
#define CM_R3L2 X
#define CM_R3L3 C
#define CM_R3L4 V
#define CM_R3L5 B
#define CM_R3R1 K
#define CM_R3R2 M
#define CM_R3R3 CMMA
#define CM_R3R4 DOT
#define CM_R3R5 FSLH

/* ── Homerow mod helpers ──
 * Use as: &bhm LSHIFT QW_R2L1  etc. Macros below expand to full 4-key holds
 * for convenience but keep &bhm explicit in keymap for readability.
 */

/* ── Shared non-alpha rows (NUM / NAV etc) ──
 * Centralize numbers & nav to unify across boards. Boards trim/&trans pad
 * where physical keys fewer.
 */
#define NUM_ROW1_F  F7 F5 F3 F1 F9  F10 F2 F4 F6 F8
#define NUM_ROW2_N  N7 N5 N3 N1 N9  N0 N2 N4 N6 N8
#define NAV_ROW1    ESC HOME UP END PLUS  NONE NONE NONE NONE DEL
#define NAV_ROW2    TAB LEFT DOWN RIGHT MINUS  NONE LCTRL LGUI LALT K_CMENU
