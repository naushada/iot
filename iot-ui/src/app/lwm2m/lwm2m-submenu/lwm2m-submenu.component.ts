import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-lwm2m-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'server'"
         (click)="select('server')">
        <clr-icon shape="host"></clr-icon>
        <span>Server</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'security'"
         (click)="select('security')">
        <clr-icon shape="lock"></clr-icon>
        <span>Security</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class Lwm2mSubmenuComponent {
  active = 'server';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
