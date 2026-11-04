local spirv = require("@lito.spirv")
local target = lito.target({ kind = "lib", name = "vecgltf" })
local inputs = {}
local content = { 'export module vecgltf.shaders;', 'import vecgltf.spirv;', 'export namespace vecgltf::shaders {' }
for _, shader in ipairs({
  { name = "background_vertex", source = "background.vert", stage = "vertex" },
  { name = "background_fragment", source = "background.frag", stage = "fragment" },
  { name = "display_vertex", source = "display.vert", stage = "vertex" },
  { name = "display_fragment", source = "display.frag", stage = "fragment" },
  { name = "bloom_fragment", source = "bloom.frag", stage = "fragment" },
  { name = "postprocess_fragment", source = "postprocess.frag", stage = "fragment" },
  { name = "model_vertex", source = "model.vert", stage = "vertex" },
  { name = "model_fragment", source = "model.frag", stage = "fragment" },
  { name = "model_opaque_fragment", source = "model.frag", stage = "fragment",
    output = "model-opaque.frag", definitions = { "BLEND_OPAQUE=1" } },
  { name = "model_shadow_fragment", source = "model.frag", stage = "fragment",
    output = "model-shadow.frag", definitions = { "SHADOW_CAST=1" } },
}) do
  local result = spirv.compile({
    backend = "glslang", tool = lito.tool("glslang"), language = "glsl",
    source = "shaders/" .. shader.source, stage = shader.stage, target_env = "vulkan1.1",
    output = "shaders/" .. (shader.output or shader.source) .. ".spv",
    definitions = shader.definitions,
  })
  inputs[#inputs + 1] = result.spirv
  content[#content + 1] = "inline constexpr auto " .. shader.name .. " = decode({"
  content[#content + 1] = '#embed "@INPUT:' .. #inputs .. '@"'
  content[#content + 1] = "});"
end
content[#content + 1] = "}"
local source = lito.write({ output = "src/shaders.cppm", inputs = inputs, content = table.concat(content, "\n") .. "\n" })
lito.target_add_generated_source(target, source.output)
