package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FriendListScreen(vm: MainViewModel) {
    val myNick by vm.myNick.collectAsStateWithLifecycle()
    val friends by vm.friends.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("即通 · $myNick") },
                actions = {
                    TextButton(onClick = { vm.logout() }) { Text("下线") }
                },
            )
        },
    ) { padding ->
        if (friends.isEmpty()) {
            Box(
                Modifier
                    .fillMaxSize()
                    .padding(padding),
                contentAlignment = Alignment.Center,
            ) {
                Text("暂无好友（可用 im_cli 或另一台模拟器互加）", color = Color.Gray)
            }
        } else {
            LazyColumn(Modifier.padding(padding)) {
                items(friends, key = { it.id }) { friend ->
                    FriendRow(friend, onClick = { vm.openChat(friend) })
                    HorizontalDivider()
                }
            }
        }
    }
}

@Composable
private fun FriendRow(friend: Friend, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // 在线状态圆点
        Box(
            Modifier
                .size(12.dp)
                .background(
                    if (friend.online) Color(0xFF07C160) else Color(0xFFBFBFBF),
                    CircleShape,
                )
        )
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
