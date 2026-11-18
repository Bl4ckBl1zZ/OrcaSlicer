#ifndef slic3r_Mcp_Schema_hpp_
#define slic3r_Mcp_Schema_hpp_

// JSON Schema (2020-12) subset used for tool arguments: validation with messages an agent can act
// on, the shared "$defs" fragments, and a helper to write schemas as JSON literals.
//
// Supported keywords: type (string or array of strings), enum, const, properties, required,
// additionalProperties (bool or schema), items, minItems, maxItems, minimum, maximum,
// exclusiveMinimum, exclusiveMaximum, minLength, maxLength, pattern, oneOf, anyOf, allOf, not,
// $ref to "#/$defs/<Name>". Annotation keywords (description, default, title, examples) are ignored
// by the validator and passed through to the client.
//
// No wx and no GUI dependencies: unit-testable.

#include <string>

#include "McpJson.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Schema {

// Parses a schema written as a JSON literal. Throws std::runtime_error naming `what` when the text
// is not valid JSON, so a typo fails loudly at start-up instead of at the first call.
json parse(const char* text, const char* what = "schema");

// The shared fragments: Id, PlateRef, Vec3, Vec2, Frame, Color, Scope, Camera.
const json& shared_defs();

// Returns `schema` with a "$defs" member holding every shared fragment it references, directly or
// through another fragment. Fragments the schema defines itself win over the shared ones.
json with_shared_defs(json schema);

// Validates `value` against `schema`; `schema` is also the root "$ref" resolves against.
// Returns an empty string when valid, otherwise one sentence naming the argument path, what is
// wrong and what is accepted, e.g.
//   `camera.zoom` must be > 0 (got -1).
//   Unknown argument `colour`. Did you mean `color`? Accepted: color, slot, action.
std::string validate(const json& schema, const json& value);

// Checks that a schema only uses supported keywords and that every $ref resolves. Returns an empty
// string or a description of the first problem. Run on every tool at registration.
std::string check_schema(const json& schema);

} // namespace Schema

}}} // namespace Slic3r::GUI::Mcp

#endif
