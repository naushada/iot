import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-lwm2m-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'dm'"
         (click)="select('dm')">
        <clr-icon shape="cog"></clr-icon>
        <span>DM</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'bs'"
         (click)="select('bs')">
        <clr-icon shape="cloud"></clr-icon>
        <span>BS</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class Lwm2mSubmenuComponent {
  active = 'dm';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
