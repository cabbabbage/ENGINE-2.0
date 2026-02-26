#include "dm_styles.hpp"

namespace {
const SDL_Color kTextPrimary       = dm::rgba(234, 239, 247, 255);
const SDL_Color kTextSecondary     = dm::rgba(186, 199, 217, 255);

const SDL_Color kPanelBackground   = dm::rgba(10, 14, 26, 238);
const SDL_Color kPanelHeader       = dm::rgba(16, 23, 38, 245);
const SDL_Color kNeutralBorder     = dm::rgba(58, 76, 105, 255);
const SDL_Color kPanelBorder       = kNeutralBorder;
const SDL_Color kHighlightColor    = dm::rgba(56, 189, 248, 180);
const SDL_Color kShadowColor       = dm::rgba(3, 7, 18, 190);
constexpr float kHighlightIntensity = 0.18f;
constexpr float kShadowIntensity    = 0.28f;
constexpr int kCornerRadius         = 8;
constexpr int kBevelDepth           = 1;

const SDL_Color kHeaderBg          = dm::rgba(33, 45, 68, 238);
const SDL_Color kHeaderHover       = dm::rgba(45, 60, 88, 245);
const SDL_Color kHeaderPress       = dm::rgba(27, 38, 57, 235);
const SDL_Color kHeaderText        = kTextPrimary;

const SDL_Color kIconBg            = dm::rgba(28, 39, 58, 235);
const SDL_Color kIconHover         = dm::rgba(39, 53, 79, 242);
const SDL_Color kIconPress         = dm::rgba(23, 32, 48, 235);
const SDL_Color kIconBorder        = dm::rgba(73, 94, 128, 255);
const SDL_Color kIconText          = kTextPrimary;

const SDL_Color kAccentBase            = dm::rgba(14, 165, 233, 235);
const SDL_Color kAccentHover           = dm::rgba(56, 189, 248, 242);
const SDL_Color kAccentPress           = dm::rgba(12, 148, 196, 235);
const SDL_Color kAccentBorder          = dm::rgba(12, 131, 185, 255);
const SDL_Color kAccentText            = dm::rgba(240, 249, 255, 255);

const SDL_Color kFooterToggleBg     = kAccentBase;
const SDL_Color kFooterToggleHover  = kAccentHover;
const SDL_Color kFooterTogglePress  = kAccentPress;
const SDL_Color kFooterToggleBorder = kAccentBorder;
const SDL_Color kFooterToggleText   = kAccentText;

const SDL_Color kWarnBg            = dm::rgba(248, 180, 28, 235);
const SDL_Color kWarnHover         = dm::rgba(250, 204, 21, 242);
const SDL_Color kWarnPress         = dm::rgba(202, 138, 4, 235);
const SDL_Color kWarnBorder        = dm::rgba(161, 98, 7, 255);
const SDL_Color kWarnText          = dm::rgba(36, 36, 36, 255);

const SDL_Color kListBg            = dm::rgba(31, 44, 64, 230);
const SDL_Color kListHover         = dm::rgba(44, 58, 82, 238);
const SDL_Color kListPress         = dm::rgba(25, 35, 52, 235);
const SDL_Color kListBorder        = kNeutralBorder;
const SDL_Color kListText          = kTextPrimary;

const SDL_Color kCreateBg          = dm::rgba(22, 163, 74, 235);
const SDL_Color kCreateHover       = dm::rgba(34, 197, 94, 242);
const SDL_Color kCreatePress       = dm::rgba(21, 128, 61, 235);
const SDL_Color kCreateBorder      = dm::rgba(18, 110, 54, 255);
const SDL_Color kCreateText        = dm::rgba(234, 247, 238, 255);

const SDL_Color kDeleteBg          = dm::rgba(239, 68, 68, 235);
const SDL_Color kDeleteHover       = dm::rgba(248, 113, 113, 242);
const SDL_Color kDeletePress       = dm::rgba(200, 54, 54, 235);
const SDL_Color kDeleteBorder      = dm::rgba(153, 27, 27, 255);
const SDL_Color kDeleteText        = dm::rgba(255, 241, 241, 255);

const SDL_Color kTextboxBg             = dm::rgba(14, 21, 34, 235);
const SDL_Color kTextboxBgHover        = dm::rgba(19, 28, 43, 240);
const SDL_Color kTextboxBorder         = dm::rgba(52, 68, 94, 255);
const SDL_Color kTextboxBorderHot      = kAccentBorder;
const SDL_Color kTextboxBorderPreview  = dm::rgba(103, 232, 249, 210);
const SDL_Color kTextboxBorderActive   = kAccentHover;
const SDL_Color kTextboxCaret          = dm::rgba(94, 234, 212, 255);
const SDL_Color kTextboxSelection      = dm::rgba(56, 189, 248, 96);
const SDL_Color kTextboxText           = kTextPrimary;

const SDL_Color kCheckboxBg            = dm::rgba(17, 25, 38, 235);
const SDL_Color kCheckboxBgHover       = dm::rgba(25, 36, 52, 240);
const SDL_Color kCheckboxOutline       = kTextboxBorder;
const SDL_Color kCheckboxCheck         = kAccentHover;
const SDL_Color kCheckboxFocus         = kAccentHover;
const SDL_Color kCheckboxHoverOutline  = kTextboxBorderPreview;
const SDL_Color kCheckboxActiveOutline = kAccentBorder;

const SDL_Color kSliderTrack           = dm::rgba(20, 28, 44, 225);
const SDL_Color kSliderFill            = dm::rgba(94, 234, 212, 190);
const SDL_Color kSliderFillActive      = kAccentHover;
const SDL_Color kSliderKnob            = dm::rgba(226, 232, 240, 255);
const SDL_Color kSliderKnobHover       = dm::rgba(241, 245, 249, 255);
const SDL_Color kSliderKnobBorder      = dm::rgba(96, 115, 148, 255);
const SDL_Color kSliderKnobBorderHover = kAccentHover;
const SDL_Color kSliderKnobAccent      = kAccentHover;
const SDL_Color kSliderKnobAccentBorder = kAccentBorder;
const SDL_Color kSliderFocusOutline    = kAccentHover;
const SDL_Color kSliderHoverOutline    = kSliderKnobBorderHover;

const SDL_Color kButtonFocusOutline = kAccentBorder;
const SDL_Color kButtonBaseFill     = kIconBg;
const SDL_Color kButtonHoverFill    = kIconHover;
const SDL_Color kButtonPressFill    = kIconPress;
}  // namespace

const DMLabelStyle &DMStyles::Label() {
  static const DMLabelStyle s{dm::FONT_PATH, 16, kTextPrimary};
  return s;
}

const DMButtonStyle &DMStyles::HeaderButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 17, kHeaderText},
      kHeaderBg,
      kHeaderHover,
      kHeaderPress,
      kPanelBorder,
      kHeaderText};
  return s;
}

const DMButtonStyle &DMStyles::IconButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 16, kIconText},
      kIconBg,
      kIconHover,
      kIconPress,
      kIconBorder,
      kIconText};
  return s;
}

const DMButtonStyle &DMStyles::CloseButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 16, kDeleteText},
      kDeleteBg,
      kDeleteHover,
      kDeletePress,
      kDeleteBorder,
      kDeleteText};
  return s;
}

const DMButtonStyle &DMStyles::PrimaryButton() { return DMStyles::AccentButton(); }

const DMButtonStyle &DMStyles::AccentButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 17, kAccentText},
      kAccentBase,
      kAccentHover,
      kAccentPress,
      kAccentBorder,
      kAccentText};
  return s;
}

const DMButtonStyle &DMStyles::FooterToggleButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 16, kFooterToggleText},
      kFooterToggleBg,
      kFooterToggleHover,
      kFooterTogglePress,
      kFooterToggleBorder,
      kFooterToggleText};
  return s;
}

const DMButtonStyle &DMStyles::WarnButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 17, kWarnText},
      kWarnBg,
      kWarnHover,
      kWarnPress,
      kWarnBorder,
      kWarnText};
  return s;
}

const DMButtonStyle &DMStyles::ListButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 16, kListText},
      kListBg,
      kListHover,
      kListPress,
      kListBorder,
      kListText};
  return s;
}

const DMButtonStyle &DMStyles::SecondaryButton() { return DMStyles::ListButton(); }

const DMButtonStyle &DMStyles::CreateButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 16, kCreateText},
      kCreateBg,
      kCreateHover,
      kCreatePress,
      kCreateBorder,
      kCreateText};
  return s;
}

const DMButtonStyle &DMStyles::DeleteButton() {
  static const DMButtonStyle s{
      {dm::FONT_PATH, 16, kDeleteText},
      kDeleteBg,
      kDeleteHover,
      kDeletePress,
      kDeleteBorder,
      kDeleteText};
  return s;
}

const DMTextBoxStyle &DMStyles::TextBox() {
  static const DMTextBoxStyle s{
      {dm::FONT_PATH, 14, kTextSecondary},
      kTextboxBg,
      kTextboxBorder,
      kTextboxBorderHot,
      kTextboxText};
  return s;
}

const DMCheckboxStyle &DMStyles::Checkbox() {
  static const DMCheckboxStyle s{
      {dm::FONT_PATH, 14, kTextSecondary},
      kCheckboxBg,
      kCheckboxCheck,
      kCheckboxOutline};
  return s;
}

const DMSliderStyle &DMStyles::Slider() {
  static const DMSliderStyle s{
      {dm::FONT_PATH, 14, kTextSecondary},
      {dm::FONT_PATH, 14, kTextPrimary},
      kSliderTrack,
      kSliderFill,
      kSliderFillActive,
      kSliderKnob,
      kSliderKnobHover,
      kSliderKnobBorder,
      kSliderKnobBorderHover,
      kSliderKnobAccent,
      kSliderKnobAccentBorder};
  return s;
}

const SDL_Color &DMStyles::PanelBG() {
  static const SDL_Color c = kPanelBackground;
  return c;
}

const SDL_Color &DMStyles::PanelHeader() {
  static const SDL_Color c = kPanelHeader;
  return c;
}

const SDL_Color &DMStyles::Border() {
  static const SDL_Color c = kPanelBorder;
  return c;
}

int DMStyles::CornerRadius() { return kCornerRadius; }

int DMStyles::BevelDepth() { return kBevelDepth; }

const SDL_Color &DMStyles::HighlightColor() {
  static const SDL_Color c = kHighlightColor;
  return c;
}

const SDL_Color &DMStyles::ShadowColor() {
  static const SDL_Color c = kShadowColor;
  return c;
}

float DMStyles::HighlightIntensity() { return kHighlightIntensity; }

float DMStyles::ShadowIntensity() { return kShadowIntensity; }

const SDL_Color &DMStyles::ButtonBaseFill() {
  static const SDL_Color c = kButtonBaseFill;
  return c;
}

const SDL_Color &DMStyles::ButtonHoverFill() {
  static const SDL_Color c = kButtonHoverFill;
  return c;
}

const SDL_Color &DMStyles::ButtonPressedFill() {
  static const SDL_Color c = kButtonPressFill;
  return c;
}

const SDL_Color &DMStyles::ButtonFocusOutline() {
  static const SDL_Color c = kButtonFocusOutline;
  return c;
}

const SDL_Color &DMStyles::TextboxBaseFill() {
  static const SDL_Color c = kTextboxBg;
  return c;
}

const SDL_Color &DMStyles::TextboxHoverFill() {
  static const SDL_Color c = kTextboxBgHover;
  return c;
}

const SDL_Color &DMStyles::TextboxFocusOutline() {
  static const SDL_Color c = kTextboxBorderHot;
  return c;
}

const SDL_Color &DMStyles::TextboxHoverOutline() {
  static const SDL_Color c = kTextboxBorderPreview;
  return c;
}

const SDL_Color &DMStyles::TextboxActiveOutline() {
  static const SDL_Color c = kTextboxBorderActive;
  return c;
}

const SDL_Color &DMStyles::TextCaretColor() {
  static const SDL_Color c = kTextboxCaret;
  return c;
}

const SDL_Color &DMStyles::TextboxSelectionFill() {
  static const SDL_Color c = kTextboxSelection;
  return c;
}

const SDL_Color &DMStyles::CheckboxBaseFill() {
  static const SDL_Color c = kCheckboxBg;
  return c;
}

const SDL_Color &DMStyles::CheckboxHoverFill() {
  static const SDL_Color c = kCheckboxBgHover;
  return c;
}

const SDL_Color &DMStyles::CheckboxCheckColor() {
  static const SDL_Color c = kCheckboxCheck;
  return c;
}

const SDL_Color &DMStyles::CheckboxOutlineColor() {
  static const SDL_Color c = kCheckboxOutline;
  return c;
}

const SDL_Color &DMStyles::CheckboxHoverOutline() {
  static const SDL_Color c = kCheckboxHoverOutline;
  return c;
}

const SDL_Color &DMStyles::CheckboxActiveOutline() {
  static const SDL_Color c = kCheckboxActiveOutline;
  return c;
}

const SDL_Color &DMStyles::CheckboxFocusOutline() {
  static const SDL_Color c = kCheckboxFocus;
  return c;
}

const SDL_Color &DMStyles::SliderTrackBackground() {
  static const SDL_Color c = kSliderTrack;
  return c;
}

const SDL_Color &DMStyles::SliderTrackFill() {
  static const SDL_Color c = kSliderFill;
  return c;
}

const SDL_Color &DMStyles::SliderKnobFill() {
  static const SDL_Color c = kSliderKnob;
  return c;
}

const SDL_Color &DMStyles::SliderKnobHoverFill() {
  static const SDL_Color c = kSliderKnobHover;
  return c;
}

const SDL_Color &DMStyles::SliderFocusOutline() {
  static const SDL_Color c = kSliderFocusOutline;
  return c;
}

const SDL_Color &DMStyles::SliderHoverOutline() {
  static const SDL_Color c = kSliderHoverOutline;
  return c;
}

int DMSpacing::panel_padding() { return 24; }
int DMSpacing::section_gap()   { return 24; }
int DMSpacing::item_gap()      { return 12; }
int DMSpacing::label_gap()     { return 6; }
int DMSpacing::small_gap()     { return 6; }
int DMSpacing::header_gap()    { return 16; }
