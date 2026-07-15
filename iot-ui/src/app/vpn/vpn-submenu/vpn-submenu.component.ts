import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-vpn-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'config'"
         (click)="select('config')">
        <clr-icon shape="wrench"></clr-icon>
        <span>Configuration</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'status'"
         (click)="select('status')">
        <clr-icon shape="eye"></clr-icon>
        <span>Status</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class VpnSubmenuComponent {
  active = 'config';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
