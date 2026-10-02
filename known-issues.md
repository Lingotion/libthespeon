# Known issues

## Synthesis is CPU-only
For the beta, we only support the basic CPU ONNX Runtime backend. For the full release, we will add support for GPU inference as well as custom ORT EP:s.

## `defaultEmotion` is ignored once any segment sets an emotion

`defaultEmotion` should apply to every segment that sets no emotion of its own. Currently it is used only when no segment in the document sets an emotion. Otherwise, segments without one take it from their neighbors: interpolated between the nearest segments that set one, and held constant before the first and after the last.

To get the intended result, set the emotion explicitly on each segment instead of relying on `defaultEmotion`.