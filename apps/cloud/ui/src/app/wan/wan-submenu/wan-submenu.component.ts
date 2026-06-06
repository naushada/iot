import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-wan-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'wifi'"
         (click)="select('wifi')">WiFi</a>
      <a class="subnav-tab" [class.active]="active === 'scan'"
         (click)="select('scan')">Scan Results</a>
      <a class="subnav-tab" [class.active]="active === 'priority'"
         (click)="select('priority')">Priority</a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class WanSubmenuComponent {
  active = 'wifi';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
