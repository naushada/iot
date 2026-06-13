import { Directive, DoCheck, TemplateRef, ViewContainerRef } from '@angular/core';
import { DebugService } from './debug.service';

/// Structural directive: renders its content only while Debug mode is on —
/// like `*ngIf="debug.on"` but without every host component needing to inject
/// DebugService. Re-evaluated each change-detection tick (toggling Debug in
/// the sidebar triggers CD), so the hints appear/disappear instantly.
///
/// Used on a `<clr-control-helper>` so the hint slots into Clarity's
/// below-the-input helper region and the 4-column .form-grid stays aligned.
@Directive({ selector: '[dsDebug]' })
export class DsDebugDirective implements DoCheck {
  private shown = false;

  constructor(private tpl: TemplateRef<unknown>,
              private vcr: ViewContainerRef,
              private debug: DebugService) {}

  ngDoCheck(): void {
    if (this.debug.on && !this.shown) {
      this.vcr.createEmbeddedView(this.tpl);
      this.shown = true;
    } else if (!this.debug.on && this.shown) {
      this.vcr.clear();
      this.shown = false;
    }
  }
}
