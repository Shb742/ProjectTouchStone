module stone(w, h, d,res, center = false) {
  scale([1.1, h/w, d]) sphere(r=w/2, center=center,$fn=res);
}

module TouchStoneSolid(size, res, center = false){
    
    translate([0,0,-(0.03333333333*size)]){
        difference(){
            translate([0,0,(0.2*size)]){
                stone(size*(2/3),size,0.6,res);
            }
            translate([0,0,-(size/2)+(0.03333333333*size)]){
                cube([size,size,size],center = true);
            }
         }
     }
}
module WiringHoles(size){
    translate([0,-(size*0.1333333333),size*.28]){
        cylinder(h = size*.1, r1 = 0.8, r2 = 0.8, $fn = 20);
        translate([-(size*0.15),0,0]){
            cylinder(h = size*.1, r1 = 0.8, r2 = 0.8, $fn = 20);
        }
        translate([(size*0.15),0,0]){
            cylinder(h = size*.1, r1 = 0.8, r2 = 0.8, $fn = 20);
        }
    }
    }
module TouchStoneHollow(size,res,center = false){
     difference(){
        difference(){
            difference(){
                TouchStoneSolid(size,res);
                translate([0,0,1]){
                    TouchStoneSolid(size-5,res,center=true);
                }
            }
            WiringHoles(size);
        }
        translate([0,-10,size*.08]){
            rotate([0,90,0]){
                cylinder(h = size*.5, r1 = 2.8, r2 = 2.8,$fn = 20);
            }
        }
    }
    
}
//TouchStoneHollow(80,50);
//TouchStoneHollow(160,10);
module TouchStoneBase(size,res,center = false){
    difference(){
        TouchStoneHollow(size,res);
        translate([0,0,(size*.68)]){
        cube([size,size,size],center = true);
        }
    }
}

module TouchStoneTop(size,res,center = false){
    translate([-(size/1.5),0,-(size*.17)]){
        difference(){
            TouchStoneHollow(size,res);
            translate([0,0,-(size*.32)]){
            cube([size,size,size],center = true);
            }
        }
    }
}
module TouchStone(size,res){
    TouchStoneBase(size,res);
    TouchStoneTop(size,res);
}
//TouchStone(80,80);
TouchStone(160,80);

 

