import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-routing-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'ports'"
         (click)="select('ports')">
        <clr-icon shape="forward"></clr-icon>
        <span>Port Forward</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'dnat'"
         (click)="select('dnat')">
        <clr-icon shape="target"></clr-icon>
        <span>DNAT Target</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'forward'"
         (click)="select('forward')">
        <clr-icon shape="network-globe"></clr-icon>
        <span>Forwarding</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'rules'"
         (click)="select('rules')">
        <clr-icon shape="firewall"></clr-icon>
        <span>Firewall Rules</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class RoutingSubmenuComponent {
  active = 'ports';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
