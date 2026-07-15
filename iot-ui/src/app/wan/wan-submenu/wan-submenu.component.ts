import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-wan-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'wifi'"
         (click)="select('wifi')">
        <clr-icon shape="wifi"></clr-icon>
        <span>WiFi Config</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'scan'"
         (click)="select('scan')">
        <clr-icon shape="search"></clr-icon>
        <span>Scan Results</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'priority'"
         (click)="select('priority')">
        <clr-icon shape="bars"></clr-icon>
        <span>Priority</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'cellular'"
         (click)="select('cellular')">
        <clr-icon shape="mobile"></clr-icon>
        <span>Cellular Config</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'cellstatus'"
         (click)="select('cellstatus')">
        <clr-icon shape="eye"></clr-icon>
        <span>Cellular Status</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class WanSubmenuComponent {
  active = 'wifi';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
