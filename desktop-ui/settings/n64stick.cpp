auto ControlStickSettings::construct() -> void {
  setCollapsible();
  setVisible(false);

  controlStickAdjustmentLabel.setText("Control Stick Adjustment").setFont(Font().setBold());
  controlStickAdjustmentLayout.setSize({3, 3}).setPadding(12_sx, 0);
  controlStickAdjustmentLayout.column(0).setAlignment(1.0);

  stickRangeLabel.setText("Range:");
  stickRangeValue.setAlignment(0.5);
  stickRangeSlider.setLength(86).setPosition(settings.controlstick.stickRange * 85.0).onChange([&] {
    settings.controlstick.stickRange = stickRangeSlider.position() / 85.0;
    stickRangeValue.setText({stickRangeSlider.position(), "%"});
  }).doChange();
}
