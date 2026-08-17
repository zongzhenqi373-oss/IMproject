package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FriendListScreen(vm: MainViewModel) {
    val myNick by vm.myNick.collectAsStateWithLifecycle()
    val myFeeling by vm.myFeeling.collectAsStateWithLifecycle()
    val friends by vm.friends.collectAsStateWithLifecycle()
    val myId = vm.myId

    var showProfile by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("即通") },
                actions = {
                    TextButton(onClick = { vm.logout() }) { Text("下线") }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .background(Color(0xFFF5F5F5)),
        ) {
            // 我的资料卡入口
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color.White)
                    .clickable { showProfile = true }
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Avatar(id = myId, nick = myNick, size = 52.dp)
                Spacer(Modifier.width(12.dp))
                Column {
                    Text(myNick, style = MaterialTheme.typography.titleMedium)
                    Text(myFeeling, style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
            }
            Spacer(Modifier.height(8.dp))

            if (friends.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("暂无好友（可用 im_cli 或另一台模拟器互加）", color = Color.Gray)
                }
            } else {
                LazyColumn(Modifier.background(Color.White)) {
                    items(friends, key = { it.id }) { friend ->
                        FriendRow(friend, onClick = { vm.openChat(friend) })
                        HorizontalDivider(
                            modifier = Modifier.padding(start = 76.dp),
                            thickness = 0.5.dp,
                            color = Color(0xFFE0E0E0),
                        )
                    }
                }
            }
        }
    }

    if (showProfile) {
        AlertDialog(
            onDismissRequest = { showProfile = false },
            confirmButton = {
                TextButton(onClick = { showProfile = false }) { Text("关闭") }
            },
            title = { Text("我的资料") },
            text = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Avatar(id = myId, nick = myNick, size = 64.dp)
                    Spacer(Modifier.width(16.dp))
                    Column {
                        Text(myNick, style = MaterialTheme.typography.titleLarge)
                        Spacer(Modifier.height(4.dp))
                        Text("账号：$myId", color = Color.Gray)
                        Text("签名：$myFeeling", color = Color.Gray)
                    }
                }
            },
        )
    }
}

@Composable
private fun FriendRow(friend: Friend, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box {
            Avatar(id = friend.id, nick = friend.nick, size = 52.dp)
            // 在线状态角标
            Box(
                Modifier
                    .size(14.dp)
                    .align(Alignment.BottomEnd)
                    .background(Color.White, CircleShape)
                    .padding(2.dp)
                    .background(
                        if (friend.online) Color(0xFF07C160) else Color(0xFFBFBFBF),
                        CircleShape,
                    )
            )
        }
        Spacer(Modifier.width(12.dp))
        Column {
            Text(friend.nick, style = MaterialTheme.typography.titleMedium)
            Text(
                if (friend.online) "在线 · ${friend.feeling}" else "离线",
                style = MaterialTheme.typography.bodySmall,
                color = Color.Gray,
            )
        }
    }
}
