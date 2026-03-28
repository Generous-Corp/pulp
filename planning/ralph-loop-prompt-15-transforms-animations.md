"Implement CSS Transforms Level 1 + CSS Animations Level 1 for Pulp.

References:
- https://www.w3.org/TR/css-transforms-1/
- https://www.w3.org/TR/css-animations-1/
Tracking: planning/w3c-css-support-matrix.md
GitHub Issues: #24 (Transforms), #25 (Animations)

TRANSFORMS TO IMPLEMENT:
1. transform: translate(x, y) — offset without affecting layout
2. transform: rotate(deg) — rotation around origin
3. transform: skew(x, y) — shear transform
4. transform-origin — configurable origin point (default center)
5. Multiple transform composition: 'translate(10px,0) rotate(45deg) scale(1.1)'

ANIMATIONS TO IMPLEMENT:
1. defineKeyframes(name, [{offset, props}...]) — multi-step animation definition
2. animation-name — reference a defined keyframe
3. animation-duration — total animation time
4. animation-iteration-count — number/infinite
5. animation-direction — normal/reverse/alternate
6. animation-fill-mode — forwards/backwards/both
7. animation-play-state — running/paused
8. CSS transition shorthand parsing

ARCHITECTURE:
- Transforms: add translate_x/y, rotation, skew_x/y to View, apply in paint_all via canvas.translate/rotate/scale
- Animations: add AnimationController class that manages keyframe sequences, drives ValueAnimation per property
- Bridge: setTransform(id, 'translate(10px,20px) rotate(45deg)'), defineKeyframes(name, frames), setAnimation(id, name, opts)

TESTING:
- Transform tests: verify bounds transform correctly
- Animation tests: verify keyframe interpolation
- Screenshot: animated spinner, hover scale transitions

EACH ITERATION: build, test, screenshot, commit.
COMPLETION: Output 'TRANSFORMS AND ANIMATIONS COMPLETE'" --completion-promise "TRANSFORMS AND ANIMATIONS COMPLETE" --max-iterations 20
