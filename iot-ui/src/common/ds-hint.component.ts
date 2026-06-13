import { Component, Input } from '@angular/core';

/// Debug aid rendered just below a form input (inside a clr-control-helper):
/// shows the data-store KEY that fills the field, so an Admin building their
/// own app can see the exact ds key behind every field. The field's current
/// value already shows in the Clarity input above, so no extra value box is
/// needed. Only instantiated while Debug mode is on (see DsDebugDirective),
/// so it costs nothing when off. Mirrors the services-list `.svc-key` styling.
@Component({
  selector: 'app-ds-hint',
  template: `<code class="ds-key" [title]="key">{{ key }}</code>`,
  styles: [`
    :host { display: block; margin-top: 2px; }
    .ds-key { display: block; font-size: 11px; color: #9e9e9e; font-family: monospace; }
  `]
})
export class DsHintComponent {
  @Input() key = '';
}
