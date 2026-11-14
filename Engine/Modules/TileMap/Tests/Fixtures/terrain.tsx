<?xml version="1.0" encoding="UTF-8"?>
<tileset version="1.10" name="terrain" tilewidth="16" tileheight="16" tilecount="8" columns="4" spacing="0" margin="0">
 <image source="tiles.png" width="64" height="32"/>
 <tile id="0">
  <animation>
   <frame tileid="0" duration="250"/>
   <frame tileid="1" duration="250"/>
  </animation>
  <objectgroup>
   <object id="1" x="0" y="8" width="16" height="8"/>
  </objectgroup>
  <properties>
   <property name="slippery" type="bool" value="true"/>
  </properties>
 </tile>
 <tile id="2" probability="0.5">
  <objectgroup>
   <object id="1" x="0" y="16">
    <polygon points="0,0 16,0 16,-16"/>
   </object>
  </objectgroup>
 </tile>
</tileset>
