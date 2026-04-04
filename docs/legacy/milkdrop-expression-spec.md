# MilkDrop 2 Expression Language Specification

Reference for the gnome-milkdrop expression engine port.
Sources: butterchurn (MIT), MilkDrop 2 SDK docs, ns-eel2 semantics.

## Grammar (EBNF)

```
program     = { statement } ;
statement   = assignment ";" | expression ";" ;
assignment  = IDENT "=" expression ;
expression  = logic_or ;
logic_or    = logic_and { "|" logic_and } ;
logic_and   = comparison { "&" comparison } ;
comparison  = addition { ( "==" | "!=" | "<" | ">" | "<=" | ">=" ) addition } ;
addition    = multiplication { ( "+" | "-" ) multiplication } ;
multiplication = power { ( "*" | "/" | "%" ) power } ;
power       = unary { "^" unary } ;          /* RIGHT-ASSOCIATIVE */
unary       = "-" unary | "!" unary | postfix ;
postfix     = atom { "(" arglist ")" } ;
atom        = NUMBER | IDENT | "(" expression ")" ;
arglist     = expression { "," expression } ;
```

### Tokens

| Type     | Pattern                               | Examples               |
|----------|---------------------------------------|------------------------|
| NUMBER   | `[0-9]*\.?[0-9]+([eE][+-]?[0-9]+)?`  | `3`, `3.14`, `.5`, `1e3` |
| IDENT    | `[a-zA-Z_][a-zA-Z0-9_]*`             | `zoom`, `q1`, `bass_att` |
| OP       | `+  -  *  /  %  ^`                   |                        |
| ASSIGN   | `=`                                   |                        |
| COMPARE  | `==  !=  <  >  <=  >=`                |                        |
| LOGIC    | `&  \|  !`                            |                        |
| LPAREN   | `(`                                   |                        |
| RPAREN   | `)`                                   |                        |
| COMMA    | `,`                                   |                        |
| SEMI     | `;`                                   |                        |

Whitespace (space, tab, newline) is ignored between tokens.
Trailing semicolons are optional on the last statement.
Empty input produces an empty program (no-op).

## Operator Precedence (low → high)

1. `;` — statement separator
2. `=` — assignment (right-associative)
3. `|` — logical OR
4. `&` — logical AND
5. `== != < > <= >=` — comparison
6. `+ -` — addition / subtraction
7. `* / %` — multiplication / division / modulo
8. `^` — power (right-associative)
9. unary `-`, `!` — negation, logical NOT
10. function call, grouping `()`

## Safety Semantics

All operations must be safe — no runtime exceptions:

| Case              | Result |
|-------------------|--------|
| Division by zero  | `0`    |
| Modulo by zero    | `0`    |
| `log(0)`          | `0`    |
| `log(negative)`   | `0`    |
| `sqrt(negative)`  | `sqrt(abs(x))` |
| `pow(x,y)` → NaN/Inf | `0` |
| Any NaN result    | `0`    |
| Any Infinity      | `0`    |

## Built-in Functions

### Math (1 argument)
| Name       | Semantics                          |
|------------|------------------------------------|
| `sin(x)`   | `Math.sin(x)`                      |
| `cos(x)`   | `Math.cos(x)`                      |
| `tan(x)`   | `Math.tan(x)`                      |
| `asin(x)`  | `Math.asin(clamp(x, -1, 1))`      |
| `acos(x)`  | `Math.acos(clamp(x, -1, 1))`      |
| `atan(x)`  | `Math.atan(x)`                     |
| `log(x)`   | `x > 0 ? Math.log(x) : 0`         |
| `log10(x)` | `x > 0 ? Math.log10(x) : 0`       |
| `exp(x)`   | safe `Math.exp(x)`                 |
| `sqrt(x)`  | `Math.sqrt(Math.abs(x))`           |
| `abs(x)`   | `Math.abs(x)`                      |
| `sign(x)`  | `x > 0 ? 1 : x < 0 ? -1 : 0`     |
| `floor(x)` | `Math.floor(x)`                    |
| `ceil(x)`  | `Math.ceil(x)`                     |
| `int(x)`   | `Math.floor(x)` (truncate toward −∞) |
| `sqr(x)`   | `x * x`                            |
| `bnot(x)`  | `Math.abs(x) < EPSILON ? 1 : 0`   |
| `invsqrt(x)` | `1 / Math.sqrt(Math.abs(x))` (safe) |

### Math (2 arguments)
| Name         | Semantics                          |
|--------------|------------------------------------|
| `atan2(y,x)` | `Math.atan2(y, x)`                |
| `pow(x,y)`   | safe `Math.pow(x, y)`             |
| `min(x,y)`   | `Math.min(x, y)`                  |
| `max(x,y)`   | `Math.max(x, y)`                  |

### Logic / Comparison (2 arguments)
| Name         | Semantics                                                |
|--------------|----------------------------------------------------------|
| `equal(x,y)` | `Math.abs(x - y) < EPSILON ? 1 : 0`                    |
| `above(x,y)` | `x > y ? 1 : 0`                                        |
| `below(x,y)` | `x < y ? 1 : 0`                                        |
| `bor(x,y)`   | `abs(x) > EPSILON \|\| abs(y) > EPSILON ? 1 : 0`       |
| `band(x,y)`  | `abs(x) > EPSILON && abs(y) > EPSILON ? 1 : 0`         |

### Bitwise (2 arguments)
| Name         | Semantics                          |
|--------------|------------------------------------|
| `bitor(x,y)` | `Math.floor(x) \| Math.floor(y)`  |
| `bitand(x,y)`| `Math.floor(x) & Math.floor(y)`   |

### Control (3 arguments)
| Name           | Semantics                                        |
|----------------|--------------------------------------------------|
| `if(x,y,z)`    | `Math.abs(x) > EPSILON ? y : z`                |
| `sigmoid(x,y)` | `t = 1 + exp(-x*y); abs(t) > EPSILON ? 1/t : 0` |

### Random
| Name       | Semantics                                    |
|------------|----------------------------------------------|
| `rand(x)`  | `x < 1 ? Math.random() : Math.random() * Math.floor(x)` |

### Memory
| Name            | Semantics                                  |
|-----------------|--------------------------------------------|
| `megabuf(i)`    | Per-preset large array (1M entries), read/write |
| `gmegabuf(i)`   | Global large array shared across presets   |

`EPSILON = 0.00001`

## Variable Tables

### Per-Frame Read-Only (set by engine each frame)

| Variable       | Description                       |
|----------------|-----------------------------------|
| `time`         | Seconds since start               |
| `frame`        | Frame counter                     |
| `fps`          | Current frames per second          |
| `progress`     | Preset progress [0,1]             |
| `bass`         | Current bass level                 |
| `mid`          | Current mid level                  |
| `treb`         | Current treble level               |
| `bass_att`     | Attenuated bass (smoothed)         |
| `mid_att`      | Attenuated mid (smoothed)          |
| `treb_att`     | Attenuated treble (smoothed)       |
| `meshx`        | Mesh grid width                    |
| `meshy`        | Mesh grid height                   |
| `aspectx`      | 1/aspect ratio x                   |
| `aspecty`      | 1/aspect ratio y                   |
| `pixelsx`      | Texture width in pixels            |
| `pixelsy`      | Texture height in pixels           |
| `rand_start`   | 4 random floats, fixed at start    |
| `rand_preset`  | 4 random floats, fixed per preset  |

### Per-Frame Read-Write (preset equations modify these)

| Variable      | Default | Description                       |
|---------------|---------|-----------------------------------|
| `zoom`        | 1.0     | Zoom amount                       |
| `zoomexp`     | 1.0     | Zoom exponent                     |
| `rot`         | 0.0     | Rotation (radians/frame)          |
| `warp`        | 1.0     | Warp amount                       |
| `cx`          | 0.5     | Center of rotation X              |
| `cy`          | 0.5     | Center of rotation Y              |
| `dx`          | 0.0     | X translation per frame           |
| `dy`          | 0.0     | Y translation per frame           |
| `sx`          | 1.0     | X stretch                         |
| `sy`          | 1.0     | Y stretch                         |
| `decay`       | 0.98    | Feedback decay                    |
| `wave_mode`   | 0       | Waveform type (0-7)               |
| `wave_a`      | 0.8     | Waveform alpha                    |
| `wave_r`      | 1.0     | Waveform red                      |
| `wave_g`      | 1.0     | Waveform green                    |
| `wave_b`      | 1.0     | Waveform blue                     |
| `wave_x`      | 0.5     | Waveform center X                 |
| `wave_y`      | 0.5     | Waveform center Y                 |
| `wave_scale`  | 1.0     | Waveform scale                    |
| `wave_smoothing` | 0.75 | Waveform smoothing                |
| `wave_mystery`| 0.0     | Waveform mystery parameter        |
| `ob_size`     | 0.01    | Outer border size                 |
| `ob_r`        | 0.0     | Outer border red                  |
| `ob_g`        | 0.0     | Outer border green                |
| `ob_b`        | 0.0     | Outer border blue                 |
| `ob_a`        | 0.0     | Outer border alpha                |
| `ib_size`     | 0.01    | Inner border size                 |
| `ib_r`        | 0.25    | Inner border red                  |
| `ib_g`        | 0.25    | Inner border green                |
| `ib_b`        | 0.25    | Inner border blue                 |
| `ib_a`        | 0.0     | Inner border alpha                |
| `mv_x`        | 12.0    | Motion vector grid X count        |
| `mv_y`        | 9.0     | Motion vector grid Y count        |
| `mv_dx`       | 0.0     | Motion vector X offset            |
| `mv_dy`       | 0.0     | Motion vector Y offset            |
| `mv_l`        | 0.9     | Motion vector length              |
| `mv_r`        | 1.0     | Motion vector red                 |
| `mv_g`        | 1.0     | Motion vector green               |
| `mv_b`        | 1.0     | Motion vector blue                |
| `mv_a`        | 0.0     | Motion vector alpha               |
| `b1n`         | 0.0     | Blur level 1 min                  |
| `b2n`         | 0.0     | Blur level 2 min                  |
| `b3n`         | 0.0     | Blur level 3 min                  |
| `b1x`         | 1.0     | Blur level 1 max                  |
| `b2x`         | 1.0     | Blur level 2 max                  |
| `b3x`         | 1.0     | Blur level 3 max                  |
| `b1ed`        | 0.25    | Blur 1 edge darken                |
| `darken_center` | 0     | Darken center flag                |
| `gamma`       | 1.0     | Gamma correction                  |
| `echo_zoom`   | 1.0     | Echo effect zoom                  |
| `echo_alpha`  | 0.0     | Echo effect alpha                 |
| `echo_orient` | 0       | Echo effect orientation            |
| `invert`      | 0       | Invert colors flag                |
| `brighten`    | 0       | Brighten flag                     |
| `darken`      | 0       | Darken flag                       |
| `solarize`    | 0       | Solarize flag                     |
| `wrap`        | 1       | Texture wrapping flag             |
| `additivewave`| 0       | Additive waveform blending        |
| `wave_dots`   | 0       | Waveform dots flag                |
| `wave_thick`  | 0       | Waveform thick flag               |
| `monitor`     | 0.0     | Debug monitor value               |

### Q Variables (q1–q32)

Shared between per-frame and per-pixel. Persist across frames.
Written by per-frame equations, read by per-pixel.

### T Variables (t1–t8)

Per-pixel temporaries. Reset to 0 at each vertex.

### Reg Variables (reg00–reg99)

General-purpose registers. Persist across frames. Shared between per-frame, shapes, and waves.

### Per-Pixel Read-Only

| Variable | Description |
|----------|-------------|
| `x`      | Normalised X [0,1] |
| `y`      | Normalised Y [0,1] |
| `rad`    | Distance from center |
| `ang`    | Angle from center (radians) |

### Per-Pixel Read-Write

| Variable | Description |
|----------|-------------|
| `dx`     | X warp offset |
| `dy`     | Y warp offset |
| `zoom`   | Per-pixel zoom (inherited from per-frame default) |
| `zoomexp`| Per-pixel zoom exponent |
| `rot`    | Per-pixel rotation |
| `warp`   | Per-pixel warp |
| `cx`     | Per-pixel center X |
| `cy`     | Per-pixel center Y |
| `sx`     | Per-pixel stretch X |
| `sy`     | Per-pixel stretch Y |

## Custom Shapes (4 slots: shape[0]–shape[3])

### Base Values
`enabled`, `sides`, `additive`, `thickOutline`, `textured`, `num_inst`,
`x`, `y`, `rad`, `ang`, `tex_ang`, `tex_zoom`,
`r`, `g`, `b`, `a`, `r2`, `g2`, `b2`, `a2`,
`border_r`, `border_g`, `border_b`, `border_a`

### Equations
- `init_eqs`: runs once at preset load
- `frame_eqs`: runs every frame (per instance if `num_inst > 1`)

Can read q1-q32. Can read/write t1-t8 (isolated per shape, per instance).

## Custom Waves (4 slots: wave[0]–wave[3])

### Base Values
`enabled`, `samples`, `sep`, `bSpectrum`, `bUseDots`, `bDrawThick`, `bAdditive`,
`scaling`, `smoothing`, `r`, `g`, `b`, `a`

### Equations
- `init_eqs`: runs once at preset load
- `frame_eqs`: runs every frame
- `point_eqs`: runs per sample point

Per-point read-only: `sample`, `value1`, `value2` (audio data)
Per-point read-write: `x`, `y`, `r`, `g`, `b`, `a`
Can read q1-q32 and t1-t8.

## Preset Object Shape (our JSON format)

```javascript
{
  id: 'expr:geiss-eggs',
  name: 'Geiss - Eggs',
  source: 'expression',

  // MilkDrop base values (defaults for per-frame variables)
  baseVals: {
    decay: 0.97, zoom: 1.046, rot: 0.02,
    warp: 1.42, wave_mode: 2, wave_a: 3.5,
    // ... any per-frame read-write variable
  },

  // Expression code strings
  init_eqs: 'q1 = 0.5; q2 = 0;',
  frame_eqs: 'zoom = zoom + 0.023 * sin(0.339 * time); rot = rot + 0.03 * sin(0.381 * time);',
  pixel_eqs: 'zoom = zoom + 0.27 * sin(time * 1.55 + rad * 5);',

  // Optional GLSL (warp shader, comp shader)
  warp: '',
  comp: '',

  // Custom shapes (up to 4)
  shapes: [
    {
      baseVals: { enabled: 1, sides: 4, x: 0.5, y: 0.5, rad: 0.1, r: 1, g: 0, b: 0, a: 0.8 },
      init_eqs: '',
      frame_eqs: 'ang = time * 0.5;',
    },
    // ... up to 4
  ],

  // Custom waves (up to 4)
  waves: [
    {
      baseVals: { enabled: 1, samples: 512, sep: 0, scaling: 1.0, smoothing: 0.5 },
      init_eqs: '',
      frame_eqs: 'r = 0.5 + sin(time); sep = bass * 100;',
      point_eqs: 'x = sample * 2 - 1; y = value1 * scaling;',
    },
    // ... up to 4
  ],
}
```

Detection: a preset with `init_eqs`, `frame_eqs`, or `pixel_eqs` string fields
is an expression-based preset. A preset with `frame` object fields is a legacy
WaveSpec preset. The evaluator auto-detects and dispatches.
