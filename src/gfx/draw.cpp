#include "draw.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gfx {
namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr float tau = 2 * pi;
constexpr float halfFeather = 0.5f;
constexpr int maxCornerSegments = 64;
constexpr int maxContourPoints = 4 * (maxCornerSegments + 1);

const std::array<SDL_FPoint, maxCornerSegments + 1> quarterCircle = [] {
	std::array<SDL_FPoint, maxCornerSegments + 1> points;
	for (int i = 0; i <= maxCornerSegments; ++i) {
		const float angle = (pi * 0.5f) * static_cast<float>(i) / maxCornerSegments;
		points[i] = {std::cos(angle), std::sin(angle)};
	}
	points.front() = {1, 0};
	points.back() = {0, 1};
	return points;
}();

int cornerSegments(float radius) {
	int segments = 1;
	// Keep chord error below about 0.16 px at ordinary window sizes.
	while (segments < maxCornerSegments && radius > 0.5f * segments * segments)
		segments *= 2;
	return segments;
}

SDL_FColor floatColor(SDL_Color color) {
	constexpr float scale = 1.0f / 255;
	return {color.r * scale, color.g * scale, color.b * scale, color.a * scale};
}

SDL_FColor mix(SDL_FColor a, SDL_FColor b, float amount) {
	return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount, a.b + (b.b - a.b) * amount,
	        a.a + (b.a - a.a) * amount};
}

SDL_FColor gradientAt(const SDL_FRect& rect, const std::array<SDL_FColor, 4>& colors, SDL_FPoint point) {
	const float x = std::clamp((point.x - rect.x) / rect.w, 0.0f, 1.0f);
	const float y = std::clamp((point.y - rect.y) / rect.h, 0.0f, 1.0f);
	return mix(mix(colors[0], colors[1], x), mix(colors[3], colors[2], x), y);
}

SDL_FRect inset(const SDL_FRect& rect, float amount) {
	return {rect.x + amount, rect.y + amount, rect.w - 2 * amount, rect.h - 2 * amount};
}

SDL_FPoint cornerPoint(const SDL_FRect& rect, float radius, int corner, SDL_FPoint direction) {
	switch (corner) {
	case 0:
		return {rect.x + rect.w - radius + radius * direction.y, rect.y + radius - radius * direction.x};
	case 1:
		return {rect.x + rect.w - radius + radius * direction.x, rect.y + rect.h - radius + radius * direction.y};
	case 2:
		return {rect.x + radius - radius * direction.y, rect.y + rect.h - radius + radius * direction.x};
	default:
		return {rect.x + radius - radius * direction.x, rect.y + radius - radius * direction.y};
	}
}

void setColor(SDL_Renderer& renderer, SDL_Color color) {
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(&renderer, color.r, color.g, color.b, color.a);
}

void setClip(SDL_Renderer& renderer, const SDL_FRect& rect) {
	const int left = static_cast<int>(std::floor(rect.x));
	const int top = static_cast<int>(std::floor(rect.y));
	// Round outwards, but preserve an empty float intersection as an empty clip.
	const SDL_Rect clip{left, top, rect.w > 0 ? static_cast<int>(std::ceil(rect.x + rect.w)) - left : 0,
	                    rect.h > 0 ? static_cast<int>(std::ceil(rect.y + rect.h)) - top : 0};
	SDL_SetRenderClipRect(&renderer, &clip);
}

} // namespace

void fill(SDL_Renderer& renderer, const SDL_FRect& rect, SDL_Color color) {
	if (rect.w <= 0 || rect.h <= 0 || color.a == 0)
		return;
	setColor(renderer, color);
	SDL_RenderFillRect(&renderer, &rect);
}

void shadeAround(SDL_Renderer& renderer, const SDL_FRect& bounds, const SDL_FRect& hole, float feather, Uint8 alpha) {
	if (alpha == 0)
		return;
	const SDL_FRect outer{hole.x - feather, hole.y - feather, hole.w + feather * 2, hole.h + feather * 2};
	// The ring between the hole and `outer` ramps from clear to the full shade; its corners
	// are mitred, so the falloff stays continuous around them.
	const SDL_FColor clear{0, 0, 0, 0}, dark{0, 0, 0, alpha / 255.0f};
	const std::array<SDL_Vertex, 8> vertices{{
	    {{hole.x, hole.y}, clear, {}},
	    {{hole.x + hole.w, hole.y}, clear, {}},
	    {{hole.x + hole.w, hole.y + hole.h}, clear, {}},
	    {{hole.x, hole.y + hole.h}, clear, {}},
	    {{outer.x, outer.y}, dark, {}},
	    {{outer.x + outer.w, outer.y}, dark, {}},
	    {{outer.x + outer.w, outer.y + outer.h}, dark, {}},
	    {{outer.x, outer.y + outer.h}, dark, {}},
	}};
	constexpr std::array<int, 24> indices{0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
	SDL_RenderGeometry(&renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(),
	                   static_cast<int>(indices.size()));
	const SDL_Color solid{0, 0, 0, alpha};
	const float right = bounds.x + bounds.w, bottom = bounds.y + bounds.h;
	fill(renderer, {bounds.x, bounds.y, bounds.w, outer.y - bounds.y}, solid);
	fill(renderer, {bounds.x, outer.y + outer.h, bounds.w, bottom - outer.y - outer.h}, solid);
	fill(renderer, {bounds.x, outer.y, outer.x - bounds.x, outer.h}, solid);
	fill(renderer, {outer.x + outer.w, outer.y, right - outer.x - outer.w, outer.h}, solid);
}

void line(SDL_Renderer& renderer, SDL_FPoint from, SDL_FPoint to, SDL_Color color, float thickness) {
	if (thickness <= 0 || color.a == 0)
		return;
	if (thickness <= 1) {
		color.a = static_cast<Uint8>(std::lround(color.a * thickness));
		setColor(renderer, color);
		SDL_RenderLine(&renderer, from.x, from.y, to.x, to.y);
		return;
	}
	const float dx = to.x - from.x;
	const float dy = to.y - from.y;
	const float length = std::hypot(dx, dy);
	if (length == 0) {
		fill(renderer, {from.x - thickness * 0.5f, from.y - thickness * 0.5f, thickness, thickness}, color);
		return;
	}
	const SDL_FPoint normal{-dy * thickness * 0.5f / length, dx * thickness * 0.5f / length};
	const SDL_FColor tint = floatColor(color);
	const std::array<SDL_Vertex, 4> vertices{{{{from.x + normal.x, from.y + normal.y}, tint, {}},
	                                          {{to.x + normal.x, to.y + normal.y}, tint, {}},
	                                          {{to.x - normal.x, to.y - normal.y}, tint, {}},
	                                          {{from.x - normal.x, from.y - normal.y}, tint, {}}}};
	constexpr std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(&renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(),
	                   static_cast<int>(indices.size()));
}

void rounded(SDL_Renderer& renderer, const SDL_FRect& rect, float radius, SDL_Color color) {
	if (rect.w <= 0 || rect.h <= 0 || color.a == 0)
		return;
	const float halfSize = std::min(rect.w, rect.h) * 0.5f;
	radius = std::clamp(radius, 0.0f, halfSize);
	const float coreInset = std::min(halfFeather, halfSize);
	const SDL_FRect core = inset(rect, coreInset);
	const SDL_FRect fringe = inset(rect, -halfFeather);
	const float coreRadius = std::max(0.0f, radius - coreInset);
	const float fringeRadius = radius + halfFeather;
	const int segments = cornerSegments(fringeRadius);
	const int points = 4 * (segments + 1);
	std::array<SDL_Vertex, 1 + 2 * maxContourPoints> vertices;
	std::array<int, 9 * maxContourPoints> indices;
	SDL_FColor solid = floatColor(color);
	solid.a *= std::min(1.0f, 2 * halfSize);
	SDL_FColor transparent = solid;
	transparent.a = 0;
	vertices[0] = {{rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f}, solid, {}};
	int point = 0;
	for (int corner = 0; corner < 4; ++corner) {
		for (int step = 0; step <= segments; ++step, ++point) {
			const SDL_FPoint direction = quarterCircle[step * maxCornerSegments / segments];
			vertices[1 + 2 * point] = {cornerPoint(core, coreRadius, corner, direction), solid, {}};
			vertices[2 + 2 * point] = {cornerPoint(fringe, fringeRadius, corner, direction), transparent, {}};
		}
	}
	int index = 0;
	for (int i = 0; i < points; ++i) {
		const int inner = 1 + 2 * i;
		const int next = 1 + 2 * ((i + 1) % points);
		for (const int value : {0, inner, next, inner, inner + 1, next + 1, inner, next + 1, next})
			indices[index++] = value;
	}
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(&renderer, nullptr, vertices.data(), 1 + 2 * points, indices.data(), index);
}

void roundedBorder(SDL_Renderer& renderer, const SDL_FRect& rect, float radius, SDL_Color color, float thickness) {
	if (rect.w <= 0 || rect.h <= 0 || thickness <= 0 || color.a == 0)
		return;
	const float halfSize = std::min(rect.w, rect.h) * 0.5f;
	if (thickness >= halfSize) {
		rounded(renderer, rect, radius, color);
		return;
	}
	radius = std::clamp(radius, 0.0f, halfSize);
	const float edgeInset = std::min(halfFeather, thickness * 0.5f);
	const std::array<float, 4> insets{-halfFeather, edgeInset, thickness - edgeInset,
	                                  std::min(halfSize, thickness + halfFeather)};
	std::array<SDL_FRect, 4> contours;
	std::array<float, 4> radii;
	for (int i = 0; i < 4; ++i) {
		contours[i] = inset(rect, insets[i]);
		radii[i] = std::max(0.0f, radius - insets[i]);
	}
	SDL_FColor solid = floatColor(color);
	// Subpixel strokes retain their coverage when the two solid contours meet.
	solid.a *= std::min(1.0f, 2 * thickness / (1 + thickness));
	std::array<SDL_FColor, 4> colors{solid, solid, solid, solid};
	colors[0].a = 0;
	colors[3].a *= std::max(0.0f, thickness + halfFeather - halfSize);
	const int segments = cornerSegments(radii[0]);
	const int points = 4 * (segments + 1);
	std::array<SDL_Vertex, 4 * maxContourPoints> vertices;
	std::array<int, 18 * maxContourPoints> indices;
	int point = 0;
	for (int corner = 0; corner < 4; ++corner) {
		for (int step = 0; step <= segments; ++step, ++point) {
			const SDL_FPoint direction = quarterCircle[step * maxCornerSegments / segments];
			for (int band = 0; band < 4; ++band)
				vertices[4 * point + band] = {
				    cornerPoint(contours[band], radii[band], corner, direction), colors[band], {}};
		}
	}
	int index = 0;
	for (int i = 0; i < points; ++i) {
		const int next = 4 * ((i + 1) % points);
		for (int band = 0; band < 3; ++band) {
			const int a = 4 * i + band;
			const int b = next + band;
			for (const int value : {a, b, b + 1, a, b + 1, a + 1})
				indices[index++] = value;
		}
	}
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(&renderer, nullptr, vertices.data(), 4 * points, indices.data(), index);
}

void gradientRounded(SDL_Renderer& renderer, const SDL_FRect& rect, float radius,
                     const std::array<SDL_Color, 4>& colors) {
	if (rect.w <= 0 || rect.h <= 0)
		return;
	constexpr int columns = 8;
	constexpr int middleSegments = 16;
	constexpr int rowWidth = columns + 3;
	constexpr int maxRows = 2 * (maxCornerSegments + 1) + middleSegments - 1;
	constexpr int maxVertices = maxRows * rowWidth + 2 * (columns + 1);
	constexpr int maxIndices = (maxRows - 1) * (columns + 2) * 6 + 2 * columns * 6;
	const float halfSize = std::min(rect.w, rect.h) * 0.5f;
	radius = std::clamp(radius, 0.0f, halfSize);
	const float coreInset = std::min(halfFeather, halfSize);
	const SDL_FRect core = inset(rect, coreInset);
	const SDL_FRect fringe = inset(rect, -halfFeather);
	const float coreRadius = std::max(0.0f, radius - coreInset);
	const float fringeRadius = radius + halfFeather;
	const int segments = cornerSegments(fringeRadius);
	const std::array<SDL_FColor, 4> ramp{floatColor(colors[0]), floatColor(colors[1]), floatColor(colors[2]),
	                                     floatColor(colors[3])};
	std::array<SDL_Vertex, maxVertices> vertices;
	std::array<int, maxIndices> indices;
	int rows = 0;
	int index = 0;
	// A small grid avoids the diagonal colour seams of a single centre fan.
	auto row = [&](float left, float right, float y, float outerLeft, float outerRight, float outerY) {
		const int base = rows * rowWidth;
		SDL_FColor edgeColor = gradientAt(rect, ramp, {outerLeft, outerY});
		edgeColor.a = 0;
		vertices[base] = {{outerLeft, outerY}, edgeColor, {}};
		for (int column = 0; column <= columns; ++column) {
			const SDL_FPoint point{left + (right - left) * static_cast<float>(column) / columns, y};
			SDL_FColor tint = gradientAt(rect, ramp, point);
			tint.a *= std::min(1.0f, 2 * halfSize);
			vertices[base + 1 + column] = {point, tint, {}};
		}
		edgeColor = gradientAt(rect, ramp, {outerRight, outerY});
		edgeColor.a = 0;
		vertices[base + rowWidth - 1] = {{outerRight, outerY}, edgeColor, {}};
		if (rows > 0) {
			for (int column = 0; column < rowWidth - 1; ++column) {
				const int a = base - rowWidth + column;
				const int b = base + column;
				for (const int value : {a, b, b + 1, a, b + 1, a + 1})
					indices[index++] = value;
			}
		}
		++rows;
	};
	for (int step = 0; step <= segments; ++step) {
		const SDL_FPoint direction = quarterCircle[step * maxCornerSegments / segments];
		row(core.x + coreRadius - coreRadius * direction.y, core.x + core.w - coreRadius + coreRadius * direction.y,
		    core.y + coreRadius - coreRadius * direction.x, fringe.x + fringeRadius - fringeRadius * direction.y,
		    fringe.x + fringe.w - fringeRadius + fringeRadius * direction.y,
		    fringe.y + fringeRadius - fringeRadius * direction.x);
	}
	if (core.h > 2 * coreRadius) {
		for (int step = 1; step < middleSegments; ++step) {
			const float amount = static_cast<float>(step) / middleSegments;
			row(core.x, core.x + core.w, core.y + coreRadius + (core.h - 2 * coreRadius) * amount, fringe.x,
			    fringe.x + fringe.w, fringe.y + fringeRadius + (fringe.h - 2 * fringeRadius) * amount);
		}
	}
	for (int step = 0; step <= segments; ++step) {
		const SDL_FPoint direction = quarterCircle[step * maxCornerSegments / segments];
		row(core.x + coreRadius - coreRadius * direction.x, core.x + core.w - coreRadius + coreRadius * direction.x,
		    core.y + core.h - coreRadius + coreRadius * direction.y,
		    fringe.x + fringeRadius - fringeRadius * direction.x,
		    fringe.x + fringe.w - fringeRadius + fringeRadius * direction.x,
		    fringe.y + fringe.h - fringeRadius + fringeRadius * direction.y);
	}
	int vertex = rows * rowWidth;
	for (int end = 0; end < 2; ++end) {
		const int base = end == 0 ? 0 : (rows - 1) * rowWidth;
		const SDL_FPoint left = vertices[base].position;
		const SDL_FPoint right = vertices[base + rowWidth - 1].position;
		const int outer = vertex;
		for (int column = 0; column <= columns; ++column) {
			const SDL_FPoint point{left.x + (right.x - left.x) * static_cast<float>(column) / columns, left.y};
			SDL_FColor tint = gradientAt(rect, ramp, point);
			tint.a = 0;
			vertices[vertex++] = {point, tint, {}};
		}
		for (int column = 0; column < columns; ++column) {
			const int a = base + 1 + column;
			const int b = outer + column;
			for (const int value : {a, b, b + 1, a, b + 1, a + 1})
				indices[index++] = value;
		}
	}
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(&renderer, nullptr, vertices.data(), vertex, indices.data(), index);
}

void arc(SDL_Renderer& renderer, SDL_FPoint center, float radius, float start, float end, SDL_Color color,
         float thickness) {
	if (radius <= 0 || thickness <= 0 || start == end || color.a == 0)
		return;
	const float sweep = std::clamp(end - start, -tau, tau);
	const bool closed = std::abs(sweep) >= tau;
	const float sign = sweep > 0 ? 1.0f : -1.0f;
	const float length = std::abs(sweep) * radius;
	const float capAngle = closed ? 0.0f : std::min(std::abs(sweep) * 0.5f, halfFeather / radius);
	const float solidSweep = sign * std::max(0.0f, std::abs(sweep) - 2 * capAngle);
	const float outer = radius + thickness * 0.5f;
	const float inner = std::max(0.0f, radius - thickness * 0.5f);
	const float width = outer - inner;
	const float edgeInset = std::min(halfFeather, width * 0.5f);
	const std::array<float, 4> radii{outer + halfFeather, outer - edgeInset, inner > 0 ? inner + edgeInset : 0,
	                                 std::max(0.0f, inner - halfFeather)};
	SDL_FColor solid = floatColor(color);
	solid.a *= std::min(1.0f, 2 * width / (1 + width));
	if (!closed)
		solid.a *= std::min(1.0f, 2 * length / (1 + length));
	std::array<SDL_FColor, 4> colors{solid, solid, solid, solid};
	colors[0].a = 0;
	if (inner > 0)
		colors[3].a *= std::max(0.0f, halfFeather - inner);
	const int segments = std::max(1, static_cast<int>(std::ceil(4 * cornerSegments(radii[0]) * std::abs(sweep) / tau)));
	constexpr int maxColumns = 4 * maxCornerSegments + 3;
	std::array<SDL_Vertex, 4 * maxColumns> vertices;
	std::array<int, 18 * (maxColumns - 1)> indices;
	const float step = solidSweep / segments;
	const SDL_FPoint rotation{std::cos(step), std::sin(step)};
	const float solidStart = start + sign * capAngle;
	const float solidEnd = start + sweep - sign * capAngle;
	const SDL_FPoint first{std::cos(solidStart), std::sin(solidStart)};
	const SDL_FPoint last = closed ? first : SDL_FPoint{std::cos(solidEnd), std::sin(solidEnd)};
	SDL_FPoint direction = first;
	int columns = 0;
	auto column = [&](SDL_FPoint radial, float capOffset, bool transparent) {
		const SDL_FPoint tangent{-radial.y * sign, radial.x * sign};
		for (int band = 0; band < 4; ++band) {
			SDL_FColor tint = colors[band];
			if (transparent)
				tint.a = 0;
			vertices[4 * columns + band] = {{center.x + radial.x * radii[band] + tangent.x * capOffset,
			                                 center.y + radial.y * radii[band] + tangent.y * capOffset},
			                                tint,
			                                {}};
		}
		++columns;
	};
	if (!closed)
		column({std::cos(start), std::sin(start)}, -halfFeather, true);
	for (int i = 0; i <= segments; ++i) {
		if (i == segments)
			direction = last;
		column(direction, 0, false);
		if (i < segments)
			direction = {direction.x * rotation.x - direction.y * rotation.y,
			             direction.x * rotation.y + direction.y * rotation.x};
	}
	if (!closed)
		column({std::cos(start + sweep), std::sin(start + sweep)}, halfFeather, true);
	int index = 0;
	for (int i = 0; i < columns - 1; ++i) {
		for (int band = 0; band < 3; ++band) {
			const int a = 4 * i + band;
			const int b = a + 4;
			for (const int value : {a, b, b + 1, a, b + 1, a + 1})
				indices[index++] = value;
		}
	}
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(&renderer, nullptr, vertices.data(), 4 * columns, indices.data(), index);
}

SDL_FRect intersect(const SDL_FRect& a, const SDL_FRect& b) {
	const float left = std::max(a.x, b.x);
	const float top = std::max(a.y, b.y);
	return {left, top, std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - left),
	        std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - top)};
}

bool intersects(const SDL_FRect& a, const SDL_FRect& b) {
	const SDL_FRect overlap = intersect(a, b);
	return overlap.w > 0 && overlap.h > 0;
}

void ClipStack::push(const SDL_FRect& rect) {
	if (clips_.empty()) {
		// An enclosing renderer clip (a partial redraw's damage rect) bounds the whole stack and
		// comes back when it empties.
		outer_.reset();
		if (SDL_Rect clip; SDL_RenderClipEnabled(&renderer_) && SDL_GetRenderClipRect(&renderer_, &clip))
			outer_ = clip;
	}
	auto top = clips_.empty() ? rect : intersect(clips_.back(), rect);
	if (clips_.empty() && outer_)
		top = intersect(top, SDL_FRect{static_cast<float>(outer_->x), static_cast<float>(outer_->y),
		                               static_cast<float>(outer_->w), static_cast<float>(outer_->h)});
	clips_.push_back(top);
	setClip(renderer_, clips_.back());
}

void ClipStack::pop() {
	clips_.pop_back();
	if (!clips_.empty())
		setClip(renderer_, clips_.back());
	else
		SDL_SetRenderClipRect(&renderer_, outer_ ? &*outer_ : nullptr);
}

ScopedClip::ScopedClip(SDL_Renderer& renderer, const SDL_FRect& rect)
    : renderer_(renderer), enabled_(SDL_RenderClipEnabled(&renderer)) {
	if (enabled_) {
		SDL_GetRenderClipRect(&renderer_, &previous_);
		visible = intersect(rect, {static_cast<float>(previous_.x), static_cast<float>(previous_.y),
		                           static_cast<float>(previous_.w), static_cast<float>(previous_.h)});
	} else {
		SDL_Rect viewport;
		SDL_GetRenderViewport(&renderer_, &viewport);
		visible = intersect(rect, {0, 0, static_cast<float>(viewport.w), static_cast<float>(viewport.h)});
	}
	const int left = static_cast<int>(std::floor(visible.x)), top = static_cast<int>(std::floor(visible.y));
	const SDL_Rect clip{left, top, std::max(0, static_cast<int>(std::ceil(visible.x + visible.w)) - left),
	                    std::max(0, static_cast<int>(std::ceil(visible.y + visible.h)) - top)};
	SDL_SetRenderClipRect(&renderer_, &clip);
}

ScopedClip::~ScopedClip() {
	SDL_SetRenderClipRect(&renderer_, enabled_ ? &previous_ : nullptr);
}

} // namespace gfx
