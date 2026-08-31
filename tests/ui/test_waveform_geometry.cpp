// Nedit -- UI-layer tests: waveform peak columns, marker/playhead
// geometry, zoom/pan math (the UiState persistence contract from the
// original's editor-reopen SIGSEGV pitfall).

#include <ui/WaveformGeometry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace nedit;

namespace {

// 1000 frames, two channels: ch0 = rising ramp, ch1 = falling ramp.
struct TestAudio
{
    std::vector<float> ch0 { std::vector<float> (1000) };
    std::vector<float> ch1 { std::vector<float> (1000, 0.0f) };

    TestAudio()
    {
        for (int i = 0; i < 1000; ++i)
        {
            ch0[static_cast<std::size_t> (i)] = static_cast<float> (i) / 999.0f;
            ch1[static_cast<std::size_t> (i)] = 1.0f - static_cast<float> (i) / 999.0f;
        }
    }

    const float* pointers[2] {};
};

} // namespace

TEST_CASE ("waveform: full-view peaks bucket correctly")
{
    TestAudio a;
    a.pointers[0] = a.ch0.data();
    a.pointers[1] = a.ch1.data();

    state::UiState ui;   // full window

    // One column over everything -> min -? both channels merged:
    // min = 0.0 (both ramps include 0), max = 1.0.
    const auto single = ui::computeWaveformPeaks (a.pointers, 2, 1000, 0, 1000, ui, 1);
    REQUIRE (single.size() == 1);
    CHECK (single[0].min == Catch::Approx (0.0f).margin (1e-6));
    CHECK (single[0].max == Catch::Approx (1.0f).margin (1e-6));

    // Two columns: first half vs second half. Both channels merge, so the
    // first half's max comes from ch1 (falling ramp) and its min from ch0.
    const auto halves = ui::computeWaveformPeaks (a.pointers, 2, 1000, 0, 1000, ui, 2);
    REQUIRE (halves.size() == 2);
    CHECK (halves[0].max == Catch::Approx (1.0f).margin (1e-6));      // ch1 @0
    CHECK (halves[0].min == Catch::Approx (0.0f).margin (1e-6));      // ch0 @0
    CHECK (halves[1].max == Catch::Approx (a.ch0[999]).margin (1e-5));
    CHECK (halves[1].min == Catch::Approx (0.0f).margin (1e-6));   // ch1 @999

    // Monotonic ramp: column maxima must be non-decreasing.
    const auto many = ui::computeWaveformPeaks (a.pointers, 1, 1000, 0, 1000, ui, 50);
    REQUIRE (many.size() == 50);
    for (std::size_t c = 1; c < many.size(); ++c)
        CHECK (many[c].max >= many[c - 1].max);
}

TEST_CASE ("waveform: zoomed view only covers visible frames")
{
    TestAudio a;
    a.pointers[0] = a.ch0.data();
    a.pointers[1] = a.ch1.data();

    state::UiState ui;
    ui.visibleStartNorm = 0.5;   // second half of the trim
    ui.visibleEndNorm = 1.0;

    const auto peaks = ui::computeWaveformPeaks (a.pointers, 1, 1000, 0, 1000, ui, 10);

    // First column now starts around frame 500 => value ~0.5.
    CHECK (peaks[0].min > 0.45f);
    // Nothing below the window start may appear.
    for (const auto& col : peaks)
        CHECK (col.min > 0.4f);
}

TEST_CASE ("waveform: degenerate inputs")
{
    state::UiState ui;
    CHECK (ui::computeWaveformPeaks (nullptr, 2, 1000, 0, 1000, ui, 8).empty());
    float dummy[4] { 0.f, 0.f, 0.f, 0.f };
    const float* ptrs[1] { dummy };
    CHECK (ui::computeWaveformPeaks (ptrs, 1, 0, 0, 0, ui, 8).empty());
    CHECK (ui::computeWaveformPeaks (ptrs, 1, 4, 0, 4, ui, 0).empty());
}

TEST_CASE ("waveform: slice markers map into the visible window")
{
    // Boundary frames of slices {0,400}, {400,800}, {800,1000}.
    std::vector<std::int64_t> boundaries {
        0, 400, 800, 1000,
    };

    state::UiState ui;
    const auto full = ui::computeSliceMarkerX (boundaries, 0, 1000, ui, 200.0);
    // Boundaries 0,400,800,1000 all inside [0,1000].
    REQUIRE (full.size() == 4);
    CHECK (full[0] == Catch::Approx (0.0));
    CHECK (full[1] == Catch::Approx (80.0));
    CHECK (full[3] == Catch::Approx (200.0));

    // Half-zoom hides boundaries at 0 and anything before frame 500.
    ui.visibleStartNorm = 0.5;
    const auto half = ui::computeSliceMarkerX (boundaries, 0, 1000, ui, 200.0);
    REQUIRE (half.size() == 2);   // 800 and 1000
    CHECK (half[0] == Catch::Approx (120.0));   // (800-500)/500 * 200
    CHECK (half[1] == Catch::Approx (200.0));
}

TEST_CASE ("waveform: playhead mapping round-trips through clicks")
{
    state::UiState ui;
    ui.visibleStartNorm = 0.25;
    ui.visibleEndNorm = 0.75;

    const double width = 320.0;
    const auto frame = ui::xToFrame (160.0, 0, 1000, ui, width);   // center
    CHECK (frame == 500);

    const auto x = ui::frameToX (frame, 0, 1000, ui, width);
    CHECK (x == Catch::Approx (160.0).margin (1e-9));

    // Outside the widget: rejected.
    CHECK (ui::xToFrame (-1.0, 0, 1000, ui, width) == -1);
    CHECK (ui::xToFrame (width + 0.5, 0, 1000, ui, width) == -1);
}

TEST_CASE ("waveform: zoom keeps the anchor stationary")
{
    state::UiState ui;
    ui.visibleStartNorm = 0.25;
    ui.visibleEndNorm = 0.75;

    // Anchor at normalized 0.5 (inside the current window).
    const auto in = ui::zoomedWindow (ui, 0.5, 2.0);
    CHECK ((in.end - in.start) == Catch::Approx (0.25));
    CHECK ((in.start + in.end) / 2.0 == Catch::Approx (0.5));   // anchor unmoved

    // Zooming out clamps to the full range without exceeding it.
    const auto out = ui::zoomedWindow (ui, 0.5, 0.001);
    CHECK (out.start == Catch::Approx (0.0));
    CHECK (out.end == Catch::Approx (1.0));

    // Minimum span floor prevents infinite zoom-in.
    auto deep = ui;
    deep.visibleStartNorm = 0.0;
    deep.visibleEndNorm = 1024.0 / 1024.0;
    for (int i = 0; i < 40; ++i)
    {
        const auto w = ui::zoomedWindow (deep, 0.5, 10.0);
        deep.visibleStartNorm = w.start;
        deep.visibleEndNorm = w.end;
    }
    CHECK ((deep.visibleEndNorm - deep.visibleStartNorm)
           == Catch::Approx (ui::kMinVisibleSpanNorm));
}

TEST_CASE ("waveform: pan clamps at the edges")
{
    state::UiState ui;
    ui.visibleStartNorm = 0.0;
    ui.visibleEndNorm = 0.5;

    const auto right = ui::pannedWindow (ui, 10.0);   // far past the end
    CHECK (right.start == Catch::Approx (0.5));
    CHECK (right.end == Catch::Approx (1.0));

    ui.visibleStartNorm = right.start;
    ui.visibleEndNorm = right.end;
    const auto backLeft = ui::pannedWindow (ui, -10.0);
    CHECK (backLeft.start == Catch::Approx (0.0));
    CHECK (backLeft.end == Catch::Approx (0.5));

    ui.visibleStartNorm = 0.0;
    ui.visibleEndNorm = 0.5;

    const auto smallStep = ui::pannedWindow (ui, 0.1);
    CHECK (smallStep.start == Catch::Approx (0.1));
}
