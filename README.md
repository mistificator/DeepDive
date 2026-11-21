# Deep Dive — Procedural Underwater World Tech Demo

**Deep Dive** is a real-time, 100% procedural 3D underwater exploration tech demo built with **C++20**, **Qt 6**, and **OpenGL 3.3 Core**. Every element in the scene — terrain bathymetry, rock normal/height textures, coral ecosystems, skeletal shipwrecks, marine fauna, dynamic water optics, and the reactive stereo soundscape — is synthesized mathematically on the fly with zero external 3D models, texture assets, or audio files.

---
<img width="2560" height="1528" alt="image" src="https://github.com/user-attachments/assets/a40075ab-7ef4-4a27-b27a-13cfe30be433" />
<img width="2560" height="1528" alt="image" src="https://github.com/user-attachments/assets/7b3d1b5d-11f2-4d70-bdbf-c504fa5bd292" />
<img width="2560" height="1528" alt="image" src="https://github.com/user-attachments/assets/b510010c-8841-4d5e-ac28-2c6f28c1a6f8" />
<img width="2560" height="1528" alt="image" src="https://github.com/user-attachments/assets/68d3cde0-eed2-426e-86f4-d4f4178ffa90" />
---

## What You Will See

* **Closed 1.6 × 1.6 km Volcanic Caldera:** A sprawling, fully enclosed underwater basin (`1600 × 1600 m`) surrounded by organic fjord-like perimeter cliffs rising above the ocean surface (`y = +28 m`), preventing the diver from ever swimming into an empty void.
* **Dramatic Bathymetry & Landmarks:**
  * Intersecting winding canyons, tectonic shelf drops, layered geological terraces, and Voronoi-carved rock crevices.
  * **22 central rocky spires and sea stacks** — some submerged reefs, others piercing the ocean surface surrounded by animated shoreline foam, swirling lace, reflected ripples, and rising micro-bubbles.
  * **5 deep abyssal pits** plunging down to ~`-70 m`, where surface light and caustics fade into near-total darkness.
* **Diverse Multi-Layered Underwater Flora:**
  * Dense swaying **giant kelp forests** with animated fronds and subtle bioluminescent specks.
  * **Seagrass meadows** and **bushy Sargassum algae** carpeting gentler valleys.
  * Rigid **staghorn branching corals**, multi-tiered **table corals**, and gently swaying **gorgonian sea fans** clinging to rocky slopes and ledges.
  * **Tube sponge colonies** and **sea anemones** with animated tentacles anchored deeply into flat and gently sloped stone.
  * **Sunlit plateau greenery islands** — lush emerald-lime moss patches and broad-leafed rosettes dotting shallow terraces near the surface.
  * **Bioluminescent abyssal sea pens** softly illuminating the dark ledges inside the deep pits.
* **14 Procedural Shipwrecks (5 Distinct Archetypes):** Galleons (with raised poop decks, stern gallery windows, 3 masts, and bowsprits), sleek brigs, split-in-half hulls (with splintered planks, fallen masts, and swim-through gaps), heavily listing caravels, and wide-bellied merchant nefs. Each wreck is constructed from curved multi-segment ribs (frames), keel, keelson, deck beams, rudder, gunwales, and weathered planking with hull breaches.
* **Autonomous Marine Ecosystem with Obstacle Avoidance:**
  * **672 schooling reef fish** (28 schools) swirling around reefs, steering around boulders/wrecks, and parting naturally when the diver swims close.
  * **26 gliding manta rays** cruising mid-to-upper water layers with sinusoidal wing deformation.
  * **An autonomous 28-meter humpback whale** cruising high in open water, using multi-ray look-ahead navigation to weave gracefully around spires while singing across the caldera.
  * **5 deep-sea anglerfish** lurking at the bottom of the 5 abyssal pits. Each predator's swaying bioluminescent lure acts as a real dynamic point light illuminating the dark pit walls and needle-like teeth; when a diver descends within ~60 m, the anglerfish turns its jaws toward the player and holds a tense ambush distance (~10–14 m).
* **Physically Inspired Ocean Surface & Underwater Atmosphere:**
  * Viewed from below, a dynamic 3D ocean surface mesh renders **Snell's Window** ($\eta \approx 1.333$, critical angle $\approx 48.6^\circ$), refracted sunlight torn by ripples, glossy **total internal reflection** outside the window, and dynamic contact foam/bubbles where cliffs break the surface.
  * Spectral underwater fog, slope-aware projected caustics, volumetric god rays, and 2,200 camera-wrapped air bubbles and glowing plankton particles.
* **4 Switchable Biomes:** *Tropical Reef (Vibrant)* (default), *Kelp Reef (Emerald)*, *Purple Grotto*, and *Abyssal Trench*.
* **Live Telemetry HUD (Toggleable):** Displays GPU/VRAM info, MSAA level, shadows, PostFX, audio, gamepad status, FPS, visible chunk/LOD distribution, rendered triangle count, visible fauna per LOD, dive depth, radial position, and collision indicator.

---

## Controls

### Keyboard & Mouse

Camera look is **free-look by default** (no need to hold the left mouse button): moving the mouse inside the focused window rotates the camera seamlessly.

| Input | Action |
| :--- | :--- |
| **Mouse Move** | Free-look camera |
| **Left Click** | Re-capture mouse cursor if previously released |
| **`W` / `A` / `S` / `D`** | Swim forward / strafe left / swim backward / strafe right |
| **`Space` / `Ctrl`** | Ascend / Descend vertically |
| **`Shift`** | High-speed sprint (for traversing the 1.6 km caldera) |
| **`` ` `` / `~` / `ё`** | Toggle HUD telemetry overlay |
| **`B`** | Cycle biome palette (4 biomes) |
| **`R`** | Regenerate world with a new random seed |
| **`L`** | Toggle soft directional shadows |
| **`P`** | Toggle Post-Processing pipeline (Bloom, God Rays, lens optics, ACES, vignette) |
| **`F`** | Toggle wireframe mode (useful for inspecting terrain & prop LOD) |
| **`K`** | Summon the humpback whale nearby and trigger a whale call |
| **`M`** | Mute / unmute audio |
| **`+` (`=`) / `-`** | Increase / decrease audio volume |
| **`V`** | Toggle frame pacing timer interval |
| **`Esc`** | First press releases mouse cursor; second press exits the application |

### Gamepad (XInput & USB HID Fallback)

Supports **XInput** controllers (Xbox, 8BitDo, Steam Input, etc.) with **haptic rumble** on collisions and whale summons, plus a **Windows Multimedia (`WinMM`)** fallback for generic USB/DirectInput controllers.

| Control | Action |
| :--- | :--- |
| **Left Stick** | Swim forward / backward / strafe (analog speed with radial deadzone) |
| **Right Stick** | Look around (non-linear response curve for fine aim + fast turns) |
| **`RT` / `A`** *(Cross)* | Ascend vertically |
| **`LT` / `B`** *(Circle)* | Descend vertically |
| **`L3` or `RB`** *(R1)* | Sprint |
| **`LB`** *(L1)* | Summon humpback whale (+ haptic rumble) |
| **`Y`** *(Triangle)* | Cycle biome palette |
| **`X`** *(Square)* | Toggle Post-Processing pipeline |
| **`Back / View`** *(Share)* | Toggle HUD overlay |
| **`Start / Menu`** *(Options)* | Regenerate world with a new seed |
| **`D-Pad Left`** | Toggle shadows *(XInput)* |
| **`D-Pad Right`** | Mute / unmute audio *(XInput)* |
| **`D-Pad Up / Down`** | Audio volume + / - *(XInput)* |

---

## Mathematical & Algorithmic Techniques

### 1. Procedural Terrain, Architecture & Texture Synthesis
* **Value Noise & Fractional Brownian Motion (fBm):** Multi-octave 3D lattice [Value noise](https://en.wikipedia.org/wiki/Value_noise) blended with [Smoothstep](https://en.wikipedia.org/wiki/Smoothstep) interpolation and inter-octave domain rotations to build macro-scale seafloor undulations and organic caldera boundaries ([Fractional Brownian motion](https://en.wikipedia.org/wiki/Fractional_Brownian_motion)).
* **Domain Warping & Ridged Multifractals:** Low-frequency fBm offsets feed back into sampling coordinates to carve twisting canyon channels and coastal fjords. Inverted absolute noise octaves (`1 - |n|`, squared and weighted) create sharp underwater ridges, crags, and faceted central spires.
* **Voronoi / Worley Cellular Noise:** Distance-to-edge metrics ($F_2 - F_1$) generate sharp tectonic fissures in the terrain and fracture lines in the procedural 1024×1024 tileable rock height/normal map ([Worley noise](https://en.wikipedia.org/wiki/Worley_noise), [Voronoi diagram](https://en.wikipedia.org/wiki/Voronoi_diagram)).
* **Geological Terracing & Padded Central-Difference Normals:** Elevation values are partially quantized via smoothstep stair functions to form natural rock ledges. Each chunk evaluates a padded `51 × 51` local height cache for its `48 × 48` grid and computes normals via central [Finite differences](https://en.wikipedia.org/wiki/Finite_difference), avoiding seam discontinuities while cutting terrain build time by ~4.5× across `std::thread` workers.
* **Parametric Shipwreck Lofting:** Hull silhouettes are generated analytically from longitudinal beam curves (ogival bow tapering + transom stern narrowing), sheer lines (forecastle/poop deck lift), and curved cross-sectional ribs with classic sailing-ship tumblehome. Deterministic hashing removes planks and upper rib segments in localized breach zones to expose the internal wooden skeleton.

### 2. Chunked Multi-Tier LOD & Frustum Culling
* **4-Level Terrain LOD with Anti-Crack Skirts:** The `1600 × 1600 m` caldera is divided into `20 × 20 = 400` chunks (`80 × 80 m` each, `48 × 48` cells at LOD0 $\Rightarrow$ `1.67 m` vertex spacing). All terrain vertices reside in a single VBO, while 4 shared LOD index buffers (strides `1, 2, 4, 8`) are drawn per chunk via `glDrawElementsBaseVertex`. Downward border skirts (`14 m` drop) seal gaps between adjacent chunks at different LODs ([Level of detail](https://en.wikipedia.org/wiki/Level_of_detail_(computer_graphics))).
* **Discrete Prop & Fauna LOD + Hysteresis:** Boulders, kelp, reef flora, and all four fauna types use 2-level discrete LODs. Distance thresholds incorporate a [Hysteresis](https://en.wikipedia.org/wiki/Hysteresis) deadband ($\pm 6\text{–}12\text{ m}$) so geometry never flickers or pops when swimming near a transition boundary.
* **Gribb–Hartmann Frustum Culling:** Six normalized clip planes are extracted directly from the View-Projection matrix and tested against bounding spheres for chunks and fauna — independently for both the main camera pass and the shadow map pass ([Viewing frustum](https://en.wikipedia.org/wiki/Viewing_frustum)).

### 3. Collision Detection & Autonomous AI Steering
* **Uniform Spatial Grid ($O(1)$ Broad-Phase):** Static boulder `SphereCollider`s and shipwreck `OBBCollider`s are binned into a `20 × 20` spatial grid matching the world chunks ([Bounding volume](https://en.wikipedia.org/wiki/Bounding_volume)).
* **Sphere-vs-OBB & Terrain Sliding:** Shipwreck collisions compute the exact closest point on an Oriented Bounding Box by projecting the query sphere into the box's orthonormal local basis. Combined with analytical heightfield + 4-probe lateral cliff checks and iterative tangent velocity projection, both the diver and fauna slide smoothly along rock walls, boulders, and hulls without clipping ([Collision detection](https://en.wikipedia.org/wiki/Collision_detection)).
* **Autonomous Steering & Look-Ahead Navigation:**
  * *Fish & Manta Rays:* Home-orbiting, tangential swirling, player-fleeing (for small fish), and forward-probe obstacle avoidance inspired by [Boids](https://en.wikipedia.org/wiki/Boids).
  * *Humpback Whale:* High-altitude open-water waypoint selection paired with a 5-ray × 3-distance horizontal clearance fan (`0°`, `±24°`, `±52°` out to `76 m`), explicit central-spire keep-out fields, and strictly rate-limited yaw/pitch/bank smoothing.
  * *Abyssal Anglerfish:* Patrols the floor of its pit and transitions to an ambush state when the diver approaches within `60 m`, rotating its jaws and glowing lure toward the player while maintaining a `10–14 m` standoff distance.

### 4. Rendering, Underwater Optics & Shaders
* **GPU Hardware Instancing & Vertex-Shader Animation:** Hundreds of fish, mantas, anglerfish, and the whale are batched per LOD using `glDrawElementsInstanced` with compact per-instance buffers (`posScale`, `rotPhase`, `pitchTint`). Tail undulation, manta wing flapping, whale spine waves, kelp currents, and anglerfish lure sway are evaluated entirely in the vertex shader ([Geometry instancing](https://en.wikipedia.org/wiki/Geometry_instancing)).
* **Derivative-Corrected Triplanar Mapping & Slope-Aware Caustics:** Terrain uses triplanar [Normal mapping](https://en.wikipedia.org/wiki/Normal_mapping) with hardware [Mipmapping](https://en.wikipedia.org/wiki/Mipmap) and up to 16× [Anisotropic filtering](https://en.wikipedia.org/wiki/Anisotropic_filtering). Fragment shaders compute true triangle face normals via `dFdx(vWorldPos) × dFdy(vWorldPos)` to eliminate stretched vertical textures and caustic streaks on sheer cliffs. Animated [Caustics](https://en.wikipedia.org/wiki/Caustic_(optics)) are projected along the sun direction and attenuated smoothly by surface slope, depth, and shadows.
* **Texel-Snapped Soft Shadows (16-Tap Vogel Disk):** The orthographic shadow frustum snaps to world-space texel increments to prevent swimming-induced shadow shimmer ([Shadow mapping](https://en.wikipedia.org/wiki/Shadow_mapping)). Fragment shading performs a 4-sample blocker search to estimate penumbra radius followed by 16-tap [Percentage-closer filtering](https://en.wikipedia.org/wiki/Percentage-closer_filtering) distributed along a per-fragment-rotated Vogel spiral based on the [Golden angle](https://en.wikipedia.org/wiki/Golden_angle) ($\approx 2.4\text{ rad}$).
* **3D Ocean Surface — Snell's Window, Fresnel & Shoreline Foam:** A camera-centered concentric polar wave mesh (`72` non-linear rings × `96` sectors) displaces 4 directional ocean swells in the vertex shader. The fragment shader evaluates [Snell's law](https://en.wikipedia.org/wiki/Snell%27s_law) ($\eta = 1.3333$) and the full dielectric [Fresnel equations](https://en.wikipedia.org/wiki/Fresnel_equations): rays steeper than the critical angle ($48.6^\circ$) refract into [Snell's window](https://en.wikipedia.org/wiki/Snell%27s_window), while shallower angles undergo [Total internal reflection](https://en.wikipedia.org/wiki/Total_internal_reflection) to mirror the deep water below. A pre-water depth blit lets the water shader reconstruct the exact 3D distance to intersecting cliffs, generating surf foam lace, micro-bubbles, and concentric reflected ripples along the waterline.
* **Beer–Lambert Spectral Attenuation:** Underwater fog uses wavelength-dependent exponential transmittance $\mathbf{T}(d) = \exp(-d \cdot \sigma \cdot [1.85, 1.06, 0.86])$, absorbing red light rapidly while preserving cyan/blue clarity ([Beer–Lambert law](https://en.wikipedia.org/wiki/Beer%E2%80%93Lambert_law)).
* **Dynamic Bioluminescent Point Lights:** Each frame, the CPU mirrors the vertex shader's `rotY * rotX * rotZ` rotation and lure oscillation to upload exact world-space coordinates (`uLurePos[8]`) of nearby anglerfish lures as dynamic point lights for both environment and fauna shaders.
* **HDR Post-Processing Pipeline:** Renders into an 8× [MSAA](https://en.wikipedia.org/wiki/Multisample_anti-aliasing) `RGBA16F` [HDR](https://en.wikipedia.org/wiki/High-dynamic-range_rendering) FBO, resolves color and depth, runs a 2-iteration separable [Gaussian blur](https://en.wikipedia.org/wiki/Gaussian_blur) for [Bloom](https://en.wikipedia.org/wiki/Bloom_(shader_effect)) (skipped completely when PostFX is toggled off), composites depth-based turbidity blur, radial screen-space god rays, mask-edge [Chromatic aberration](https://en.wikipedia.org/wiki/Chromatic_aberration), subtle underwater lens refraction, vignetting, and [ACES](https://en.wikipedia.org/wiki/Academy_Color_Encoding_System) filmic [Tone mapping](https://en.wikipedia.org/wiki/Tone_mapping).

---

## Graphics & Technology Stack

| Layer | Technology / Specification |
| :--- | :--- |
| **Language & Concurrency** | C++20, multithreaded world & procedural texture generation (`std::thread`) |
| **Framework & Windowing** | Qt 6 (`Widgets`, `OpenGL`, `OpenGLWidgets`, `Multimedia`, `QPainter` HUD) |
| **Graphics API & Shaders** | OpenGL 3.3 Core Profile, GLSL `#version 330 core` (10 linked shader programs) |
| **Framebuffers & Precision** | 8× MSAA `RGBA16F` + `DEPTH24` main FBO; `RGBA16F` resolve & half-res ping-pong Bloom FBOs; `2048 × 2048` `DEPTH24` shadow map FBO |
| **Procedural Textures** | 1024×1024 `RGBA8` tileable normal/height atlas with mipmaps and up to 16× anisotropic filtering |
| **GPU Discovery & Scoring** | Win32 **DXGI 1.6** (`IDXGIFactory6::EnumAdapterByGpuPreference` with `DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE`), filtering out software adapters and ranking discrete NVIDIA/AMD GPUs by dedicated VRAM; supplemented by OpenGL `GL_GPU_MEM_TOTAL_NVX` queries |
| **Input** | Qt keyboard + locked-cursor free-look mouse; dynamically loaded Win32 `XInput` (with rumble) + `WinMM` HID fallback (`GamepadInput.h`) |
| **Audio Backend** | `Qt6::Multimedia` (`QAudioSink` + pull-mode `QIODevice` streaming 48 kHz stereo 32-bit Float / 16-bit Int PCM with a 60 ms buffer) |

### Project Structure

```text
src/
├── main.cpp              # Entry point, DXGI GPU selection & OpenGL 3.3 Core surface setup
├── GpuCapabilities.h     # DXGI 1.6 hardware GPU adapter enumeration & VRAM scoring
├── Noise.h               # Value noise, fBm, ridged multifractals, Voronoi, caldera bathymetry
├── WorldGenerator.h/.cpp # Chunked LOD terrain, 8+ flora types, 5 shipwreck archetypes, fauna, collisions
├── Shaders.h             # All GLSL 330 vertex & fragment shaders (shadows, env, fauna, water, PostFX)
├── DeepDiveWidget.h/.cpp # OpenGL renderer, simulation loop, AI steering, HUD, input handling
├── GamepadInput.h        # Zero-dependency XInput + WinMM gamepad & rumble driver
└── UnderwaterAudio.h/.cpp# Real-time lock-free procedural underwater DSP synthesizer
```

---

## Procedural Audio Synthesis

All audio is generated in real time by `UnderwaterAudioEngine` / `UnderwaterSynthDevice` (`src/UnderwaterAudio.h`, `src/UnderwaterAudio.cpp`) as a continuous 48 kHz stereo [PCM](https://en.wikipedia.org/wiki/Pulse-code_modulation) stream ([Procedural audio](https://en.wikipedia.org/wiki/Procedural_audio)).

### Lock-Free Architecture & Core DSP Building Blocks
* **Lock-Free Parameter Bridge:** The render thread publishes normalized dive depth, swim speed, active biome, volume, mute state, and manual whale-call triggers via `std::atomic` variables (`memory_order_relaxed`). Inside the audio callback (`readData`), continuous parameters pass through per-sample one-pole exponential smoothers to eliminate zipper noise and clicks.
* **Fast White Noise Generator:** A lightweight 32-bit [Xorshift](https://en.wikipedia.org/wiki/Xorshift) PRNG (`^= << 13`, `^= >> 17`, `^= << 5`) generates uncorrelated stereo white noise on the fly without mutexes or heap allocations.
* **State-Variable Filters (SVF):** Dynamic filtering uses a discrete Chamberlin [State variable filter](https://en.wikipedia.org/wiki/State_variable_filter) (`BiquadSVF`) that outputs simultaneous low-pass (`outLow`) and band-pass (`outBand`) signals in 4 floating-point operations per sample.

### Synthesis Layers
1. **Hydrostatic Ocean Rumble (Brownian Noise + Depth-Modulated SVF):**
   Stereo white noise is fed into a leaky first-order integrator ($y_n = (y_{n-1} + 0.025\,x_n)\cdot 0.975$) to produce [Brownian noise](https://en.wikipedia.org/wiki/Brownian_noise) ($1/f^2$ spectrum), then shaped by stereo SVFs ($Q = 0.65$). Dive depth closes the cutoff from ~`160 Hz` near the surface down to a sub-bass `65 Hz` rumble in the depths, while asynchronous stereo [LFOs](https://en.wikipedia.org/wiki/Low-frequency_oscillation) (`0.55 rad/s` L, `0.43 rad/s` R) create slow rolling ocean surges.
2. **Hydrodynamic Swim Whoosh (Velocity-Mapped Band-Pass Noise):**
   Stereo white noise is filtered through a second pair of SVFs ($Q = 0.45$) whose cutoff sweeps dynamically from `160 Hz` to `480 Hz` with diver velocity, blending 70% band-pass and 30% low-pass to simulate turbulent water rushing past the diving mask.
3. **Polyphonic Air Bubbles (8-Voice Exponential Chirp Synthesis):**
   An 8-voice polyphonic pool spawns stochastic bubble events at a rate scaling with swimming speed (`1.6` to `7.1` bubbles/sec). Inspired by underwater [Minnaert resonance](https://en.wikipedia.org/wiki/Minnaert_resonance), each voice renders a 2-harmonic sine burst (`1×` + `0.25×` second harmonic) over `35–90 ms` with an exponential decay envelope and a rising frequency [Chirp](https://en.wikipedia.org/wiki/Chirp) ($f(t) = f_0(1 + (1 - e(t))\cdot r \cdot 0.08)$) simulating bubble compression and collapse, randomly panned across the stereo field.
4. **Humpback Whale Vocalizations (FM + Additive Synthesis):**
   Triggered autonomously on a randomized interval (`11–22 s`) or on demand (`K` / `LB`). Each `3.2–5.6 s` call uses a $\sin^2(\pi u)$ envelope, one of three procedural pitch contours (power-law descending glide, sinusoidal arch, or undulating downward phrase), and `4.8 Hz` vibrato. The resonant vocal timbre blends [Frequency modulation synthesis](https://en.wikipedia.org/wiki/Frequency_modulation_synthesis) (carrier modulated by a detuned 2nd harmonic at `2.003×` with a time-varying modulation index) and [Additive synthesis](https://en.wikipedia.org/wiki/Additive_synthesis) (`1×`, `2.003×`, and `3.01×` partials), heavily routed into the canyon echo.
5. **Biome-Reactive Harmonic Pad (4-Voice Additive Drone):**
   Four stereo-spread sine oscillators with staggered slow LFO amplitude modulation (`0.17–0.38 rad/s`) sustain subtle underwater chords tailored to each biome:
   * *Reef (`A2 Asus2`):* `110.00, 164.81, 220.00, 246.94 Hz`
   * *Purple Grotto (`G2 / Dsus4`):* `98.00, 146.83, 220.00, 293.66 Hz`
   * *Abyssal Trench (`D2` dark fifth):* `73.42, 110.00, 146.83, 164.81 Hz`
6. **Canyon Reverb & Soft Saturation (Ping-Pong Delay + Damping + `tanh` Limiter):**
   Bubbles, whale songs, and harmonic pads feed an asymmetric stereo cross-coupled ping-pong [Delay](https://en.wikipedia.org/wiki/Delay_(audio_effect)) (`390 ms` left, `530 ms` right, `0.56` cross-feedback) to create spacious, non-periodic canyon reflections. A one-pole [Low-pass filter](https://en.wikipedia.org/wiki/Low-pass_filter) ($\alpha = 0.12$) inside each feedback loop models high-frequency acoustic absorption in water. Finally, the master stereo bus passes through a memoryless hyperbolic tangent [Waveshaper](https://en.wikipedia.org/wiki/Waveshaper) ($\tanh(x)$) acting as a smooth soft-clipping limiter.

---

## Building & Running

Requires **CMake 3.20+**, a **C++20** compiler, and **Qt 6** (`Widgets`, `OpenGL`, `OpenGLWidgets`, `Multimedia`). On Windows, `dxgi` is linked for hardware GPU enumeration (`XInput` and `WinMM` are loaded dynamically at runtime).

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/DeepDive
```
